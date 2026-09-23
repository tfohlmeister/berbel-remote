"""Tests for the hood the fake hood emulates.

Run with: python3 -m unittest discover tools/fakehood

No Bluetooth stack needed: hood.py is the protocol half and imports none.

The state machine needs no BLE: with no client connected, pushing a status
frame is a no-op, so a session can be driven entirely from here.
"""

import asyncio
import unittest

import hood as hood_module
from hood import Hood, State


def silent(_event, **_fields):
    pass


class FrameLayout(unittest.TestCase):
    """The bits a frame carries, against the masks in berbel_protocol.h."""

    def test_empty_hood_is_all_zero(self):
        self.assertEqual(Hood().frame(), bytes(9))

    def test_fan_steps_sit_where_documented(self):
        # Power is 0x09 on purpose: only 0x01 is the bit the firmware decodes,
        # but a real hood sets both, so that is what a fake hood should send.
        for length in (9, 13):
            for step, (index, mask) in {
                1: (0, 0x10), 2: (1, 0x01), 3: (1, 0x10), 4: (2, 0x09),
            }.items():
                hood = Hood()
                hood.fan = step
                frame = hood.frame(length)
                self.assertEqual(frame[index] & mask, mask, f"fan {step}/{length}")
                self.assertEqual(sum(frame), mask,
                                 f"fan {step}/{length} sets nothing else")

    def test_lights_and_afterrun(self):
        for attribute, index, mask in (
            ("light_up", 2, 0x10),
            ("light_down", 4, 0x10),
            ("light_ceiling", 5, 0x01),
            ("afterrun", 5, 0x90),
        ):
            hood = Hood()
            setattr(hood, attribute, True)
            self.assertEqual(hood.frame()[index] & mask, mask, attribute)

    def test_movement_flags(self):
        hood = Hood()
        hood.moving = "up"
        self.assertEqual(hood.frame()[4] & 0x01, 0x01)
        hood.moving = "down"
        self.assertEqual(hood.frame()[6] & 0x01, 0x01)

    def test_long_frame_moves_the_ceiling_light(self):
        """The Skyline Edge layout carries it in byte [9], not byte [5]."""
        hood = Hood()
        hood.light_ceiling = True
        self.assertEqual(hood.frame(9)[5] & 0x01, 0x01)
        long_frame = hood.frame(13)
        self.assertEqual(len(long_frame), 13)
        self.assertEqual(long_frame[9] & 0x01, 0x01)
        self.assertEqual(long_frame[5] & 0x01, 0x00)

    def test_long_frame_keeps_everything_else_in_place(self):
        hood = Hood()
        hood.fan = 2
        hood.light_up = True
        hood.light_down = True
        hood.afterrun = True
        self.assertEqual(hood.frame(13)[:9], hood.frame(9))

    def test_sync_frame_follows_the_length(self):
        self.assertEqual(Hood.sync_frame(9), bytes([0x11]) * 9)
        self.assertEqual(Hood.sync_frame(13), bytes([0x11]) * 13)


class Buttons(unittest.TestCase):
    def test_fan_buttons(self):
        hood = Hood()
        for code, step in ((0x02, 1), (0x03, 2), (0x04, 3), (0x05, 4)):
            hood.press(code)
            self.assertEqual(hood.fan, step)
        hood.press(0x01)
        self.assertEqual(hood.fan, 0)

    def test_lights_toggle(self):
        hood = Hood()
        for code, attribute in ((0x06, "light_up"), (0x0A, "light_down"),
                                (0x0B, "light_ceiling")):
            hood.press(code)
            self.assertTrue(getattr(hood, attribute), code)
            hood.press(code)
            self.assertFalse(getattr(hood, attribute), code)

    def test_ceiling_light_answers_both_of_its_codes(self):
        """The firmware drives Deckenlicht from 0x0B or, on a 7bT build, 0x12."""
        for code in (0x0B, 0x12):
            hood = Hood()
            hood.press(code)
            self.assertTrue(hood.light_ceiling, f"0x{code:02X} switches it on")
            hood.press(code)
            self.assertFalse(hood.light_ceiling, f"0x{code:02X} switches it off")

    def test_unknown_codes_report_themselves(self):
        hood = Hood()
        for code in (0x0E, 0x13, 0x14, 0x15, 0x16, 0x17):
            self.assertEqual(hood.press(code), "unhandled", f"0x{code:02X}")
        self.assertEqual(hood.frame(), bytes(9), "and change nothing")

    def test_afterrun_runs_in_fan_step_one(self):
        hood = Hood()
        hood.press(0x0C)
        self.assertTrue(hood.afterrun)
        self.assertEqual(hood.fan, 1)

    def test_lift_presses_report_a_move(self):
        hood = Hood()
        self.assertEqual(hood.press(0x09), "cover")
        self.assertEqual(hood.moving, "up")
        self.assertEqual(hood.press(0x0D), "cover")
        self.assertEqual(hood.moving, "down")


class Timers(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.cover_was = hood_module.COVER_TRAVEL_SECONDS
        hood_module.COVER_TRAVEL_SECONDS = 0.2
        self.state = State(silent, emulate=True, afterrun_seconds=0.2)

    async def asyncTearDown(self):
        hood_module.COVER_TRAVEL_SECONDS = self.cover_was

    async def test_movement_clears_on_its_own(self):
        self.state.react(0x0D)
        self.assertEqual(self.state.hood.moving, "down")
        await asyncio.sleep(0.5)
        self.assertIsNone(self.state.hood.moving)

    async def test_a_press_during_a_move_does_not_strand_the_flag(self):
        """The regression: any press cancelled the travel timer, and nothing
        else ever cleared the movement flag, so the remote showed a hood
        travelling forever."""
        self.state.react(0x0D)
        self.state.react(0x04)  # fan 3, while it is still moving
        self.assertEqual(self.state.hood.moving, "down")
        await asyncio.sleep(0.5)
        self.assertIsNone(self.state.hood.moving)
        self.assertEqual(self.state.hood.fan, 3)

    async def test_a_new_move_replaces_the_running_one(self):
        self.state.react(0x0D)
        await asyncio.sleep(0.1)
        self.state.react(0x09)
        self.assertEqual(self.state.hood.moving, "up")
        # Past the first move's deadline, well inside the restarted one.
        await asyncio.sleep(0.15)
        self.assertEqual(self.state.hood.moving, "up", "restarted, not expired")
        await asyncio.sleep(0.3)
        self.assertIsNone(self.state.hood.moving)

    async def test_afterrun_switches_the_hood_off(self):
        self.state.react(0x0C)
        self.assertTrue(self.state.hood.afterrun)
        await asyncio.sleep(0.5)
        self.assertFalse(self.state.hood.afterrun)
        self.assertEqual(self.state.hood.fan, 0)

    async def test_afterrun_can_be_cancelled_by_hand(self):
        self.state.react(0x0C)
        self.state.react(0x0C)
        self.assertFalse(self.state.hood.afterrun)
        await asyncio.sleep(0.5)
        self.assertEqual(self.state.hood.fan, 0)

    async def test_afterrun_restarts_after_an_immediate_off_and_on(self):
        """The regression: a cancelled task is not done() yet, so the press that
        switched the run-on back on saw a dead timer as still running and never
        started a new one, leaving the run-on on forever."""
        self.state.react(0x0C)
        self.state.react(0x0C)   # off, cancels the timer
        self.state.react(0x0C)   # on again, in the same loop iteration
        self.assertTrue(self.state.hood.afterrun)
        await asyncio.sleep(0.5)
        self.assertFalse(self.state.hood.afterrun, "the new timer must have run")
        self.assertEqual(self.state.hood.fan, 0)

    async def test_power_ends_a_running_afterrun(self):
        self.state.react(0x0C)
        self.state.react(0x01)
        self.assertFalse(self.state.hood.afterrun)
        await asyncio.sleep(0.5)
        self.assertEqual(self.state.hood.fan, 0)


if __name__ == "__main__":
    unittest.main()
