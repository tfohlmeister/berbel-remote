"""The hood a fake hood emulates: state, status frames and timers.

Separate from the BLE transport on purpose. Nothing here imports a Bluetooth
stack, so the part of this tool that encodes protocol knowledge can be read and
tested anywhere, the way BerbelRemote/src/berbel_protocol.h is on the firmware
side.
"""

import asyncio

# Advertised by the remote in 128-bit service data, value 0x01 while active.
ADV_SERVICE_UUID = "f000f000-5745-4053-8043-62657262656c"
BERBEL_SERVICE_UUID = "f004f000-5745-4053-8043-62657262656c"
STATUS_CHAR_UUID = "f004f001-5745-4053-8043-62657262656c"  # hood -> remote
BUTTON_CHAR_UUID = "f004f002-5745-4053-8043-62657262656c"  # remote -> hood
BATTERY_CHAR_UUID = "00002a19-0000-1000-8000-00805f9b34fb"

CHAR_ALIASES = {
    "status": STATUS_CHAR_UUID,
    "button": BUTTON_CHAR_UUID,
    "battery": BATTERY_CHAR_UUID,
}

# Seconds a lift model reports movement after a raise or lower press. The real
# travel takes about ten; this only has to outlast a look at the LEDs.
COVER_TRAVEL_SECONDS = 6.0

# The berbel manual gives the run-on time as 10 minutes in fan step 1. That is
# a long time to sit at a bench watching an LED, so the default is shortened
# and `--afterrun-seconds` sets it back to 600 for a faithful run.
AFTERRUN_SECONDS = 60.0


# --------------------------------------------------------------------------
# The hood half: enough state to answer a remote the way a hood does
# --------------------------------------------------------------------------

class Hood:
    """The state a 9-byte status frame carries, and how buttons change it.

    A remote lights its own buttons from the frames the hood writes back, so
    recording button codes is only half a hood. This is the other half: press
    a button, get the frame a real hood would have sent.

    Layout and button semantics follow BerbelRemote/src/berbel_protocol.h.
    """

    # Both frame lengths seen so far. The Skyline Edge models use the long
    # one, and move the ceiling light from byte [5] to byte [9] with it.
    FRAME_LENGTHS = (9, 13)

    @staticmethod
    def sync_frame(length=9):
        """The packet a hood opens with, which the remote is to ignore."""
        return bytes([0x11] * length)

    def __init__(self):
        self.fan = 0  # 0 off, 1..3, 4 = power
        self.light_up = False
        self.light_down = False
        self.light_ceiling = False
        self.afterrun = False
        self.moving = None  # "up", "down" or None

    def frame(self, length=9):
        frame = bytearray(length)
        if self.fan == 1:
            frame[0] |= 0x10
        elif self.fan == 2:
            frame[1] |= 0x01
        elif self.fan == 3:
            frame[1] |= 0x10
        elif self.fan == 4:
            # 0x09, although only 0x01 is the Fan Power bit the decoder tests.
            # A Skyline Frame on Power was measured sending both bits, and a
            # BFB 7bT blinks its P key for 0x09 where 0x01 alone lights it
            # steadily. Blinking is therefore the normal display for Power, and
            # sending 0x01 alone would emulate a state no hood produces.
            frame[2] |= 0x09
        if self.light_up:
            frame[2] |= 0x10
        if self.light_down:
            frame[4] |= 0x10
        if self.moving == "up":
            frame[4] |= 0x01
        if self.afterrun:
            frame[5] |= 0x90
        if self.moving == "down":
            frame[6] |= 0x01
        if self.light_ceiling:
            frame[9 if length >= 13 else 5] |= 0x01
        return bytes(frame)

    def press(self, code):
        """Apply a button press.

        Returns "cover" when the press starts a lift movement, so the caller
        knows to restart the travel timer. Timed run-on is driven off the
        `afterrun` flag instead, which the caller reconciles after every press.
        """
        if code == 0x01:  # power: off, and that ends a run-on too
            self.fan = 0
            self.afterrun = False
        elif code in (0x02, 0x03, 0x04):  # fan 1..3
            self.fan = code - 1
            self.afterrun = False
        elif code == 0x05:  # fan power
            self.fan = 4
            self.afterrun = False
        elif code == 0x06:  # Oberlicht
            self.light_up = not self.light_up
        elif code == 0x0A:  # Unterlicht
            self.light_down = not self.light_down
        elif code in (0x0B, 0x12):
            # 0x0B is the multifunction key, 0x12 the 7bT's own Uplight key.
            # The firmware drives its Deckenlicht entity from whichever of the
            # two the remote profile selects, so both have to land here or the
            # fake hood cannot answer a 7bT build at all.
            self.light_ceiling = not self.light_ceiling
        elif code == 0x0C:  # afterrun, which the manual runs in fan step 1
            self.afterrun = not self.afterrun
            self.fan = 1 if self.afterrun else 0
        elif code == 0x09:  # raise
            self.moving = "up"
            return "cover"
        elif code == 0x0D:  # lower
            self.moving = "down"
            return "cover"
        else:
            # The 7bT keys (0x0E, 0x12-0x17) and anything else: this hood has
            # no state for them, because no hood has been watched doing
            # anything with them. Saying so matters, since a silent frame is
            # indistinguishable from a bit we placed wrongly.
            return "unhandled"
        return None

    def afterrun_expired(self):
        """The run-on time is over: the hood stops."""
        self.afterrun = False
        self.fan = 0

    def describe(self):
        fan = {0: "off", 4: "power"}.get(self.fan, str(self.fan))
        return (f"fan={fan} oberlicht={self.light_up} unterlicht={self.light_down} "
                f"deckenlicht={self.light_ceiling} nachlauf={self.afterrun} "
                f"moving={self.moving}")



# --------------------------------------------------------------------------
# Session state, shared between the BLE loop and the control socket
# --------------------------------------------------------------------------

class State:
    def __init__(self, log, emulate=False, afterrun_seconds=AFTERRUN_SECONDS):
        self.log = log
        self.client = None
        self.address = None
        self.stop = asyncio.Event()
        self.emulate = emulate
        # One writer at a time: each push snapshots the frame before awaiting
        # the write, so unordered writes can leave the remote on a stale one.
        self.status_lock = asyncio.Lock()
        self.afterrun_seconds = afterrun_seconds
        self.hood = Hood()
        self.frame_length = 9
        self.advertisement = None
        self.cover_timer = None
        self.afterrun_timer = None
        self.tasks = set()

    def spawn(self, coro):
        """Run a coroutine in the background, holding a reference to it.

        asyncio keeps only a weak reference to a bare task, so one that nobody
        holds can be collected before it runs.
        """
        task = asyncio.create_task(coro)
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)
        return task

    async def push_status(self, reason):
        """Write the hood's current frame to the remote."""
        client = self.client
        if client is None or not client.is_connected:
            return
        async with self.status_lock:
            frame = self.hood.frame(self.frame_length)
            await client.write_gatt_char(STATUS_CHAR_UUID, frame, response=False)
            self.log("status", data=frame.hex(), reason=reason,
                     hood=self.hood.describe())

    async def run_cover(self, seconds):
        """Report movement for a while, then stop, the way a lift model does."""
        try:
            await asyncio.sleep(seconds)
            self.hood.moving = None
            await self.push_status("cover arrived")
        except asyncio.CancelledError:
            pass

    async def run_afterrun(self, seconds):
        """Hold the run-on for its time, then switch the hood off."""
        try:
            await asyncio.sleep(seconds)
            self.hood.afterrun_expired()
            await self.push_status("afterrun over")
        except asyncio.CancelledError:
            pass

    @staticmethod
    def cancel(timer):
        if timer and not timer.done():
            timer.cancel()

    def react(self, code):
        """Answer a button press as a hood would."""
        started = self.hood.press(code)
        if started == "unhandled":
            self.log("unhandled", code=f"0x{code:02X}",
                     note="no emulated hood behaviour, frame unchanged")

        if started == "cover":
            # Only a new move replaces a running one. Touching this timer on
            # any other press would strand the movement flag, since nothing
            # else ever clears it, and the remote would show a hood that
            # travels forever.
            self.cancel(self.cover_timer)
            self.cover_timer = self.spawn(self.run_cover(COVER_TRAVEL_SECONDS))

        running = self.afterrun_timer and not self.afterrun_timer.done()
        if self.hood.afterrun and not running:
            self.afterrun_timer = self.spawn(self.run_afterrun(self.afterrun_seconds))
        elif not self.hood.afterrun and running:
            self.cancel(self.afterrun_timer)
            # done() is still False right after cancel(), so leaving the handle
            # in place would make the next press think a timer is still running
            # and skip starting one, leaving the run-on on forever.
            self.afterrun_timer = None

        self.spawn(self.push_status(f"button 0x{code:02X}"))


