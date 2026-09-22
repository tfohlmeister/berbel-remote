# fakehood - read a Berbel remote out

A Berbel remote is the BLE **peripheral**; the hood is the **central**. So
talking to a remote directly means playing the hood. `fakehood.py` does that:
it scans for a remote, bonds with it, subscribes to the button characteristic
and records every code the remote sends. It can also write status frames back
on `f004f001`, which the remote mirrors on its own LEDs, so a frame can be
checked against what the hardware actually shows.

This is a bench tool for reverse engineering, not part of the firmware.

## Layout

| File | What it is |
|---|---|
| `hood.py` | The hood being emulated: status frames, button semantics, timers. No Bluetooth import, so it is testable anywhere. |
| `fakehood.py` | The BLE side and the CLI: scanning, pairing, GATT, the control socket. |
| `test_fakehood.py` | Tests for `hood.py`. Run with `python3 -m unittest discover -s tools/fakehood -p "test_*.py"`, no adapter needed. CI runs them. |
| `profiles/` | What real remotes look like on the air, recorded while they were here. |

## Requirements

Linux with BlueZ and a BLE adapter:

```bash
pip install bleak dbus-fast
```

No adapter preparation is needed for a first attempt. BlueZ negotiates LE
Secure Connections down to Legacy Pairing when the peer offers nothing else,
and the tool registers its own `NoInputNoOutput` agent, so Just Works
completes unattended.

If pairing fails, take Secure Connections out of the picture. The order
matters: after `bluetoothd` is up, but with the adapter powered down.

```bash
sudo btmgmt --index hci0 power off
sudo btmgmt --index hci0 sc off
sudo btmgmt --index hci0 power on
```

## Putting the remote into pairing mode

On the BFB 7bT and BFB 8bT, hold **"1" + "P"** together for more than 3
seconds; the LED starts blinking and the remote advertises. Two combinations
undo things, both held for more than 3 seconds:

| Combination | Effect |
|---|---|
| Effektbeleuchtung + Heben (▲) | Drop the current connection |
| Kochfeld-Beleuchtung + Senken (▼) | Factory reset |

Sources: berbel documents 6007021_0 (7bT) and 6007335_0 (8bT).

## Use

```bash
# What is advertising? Remotes are marked berbel=True.
python3 fakehood.py scan --seconds 20
python3 fakehood.py scan --seconds 20 --all      # every advertiser

# Record. Matches the first remote that advertises, reconnects after a drop.
python3 fakehood.py --log capture.jsonl run
python3 fakehood.py --log capture.jsonl run --address 88:01:F9:AA:BB:CC

# Record and answer: behave like a hood, so the remote's LEDs come alive.
python3 fakehood.py --log capture.jsonl run --emulate
```

Without `--emulate` the tool only listens, and a remote talking to it stays
dark: its buttons are lit from the status frames a hood writes back, and a
listener writes nothing. `--emulate` adds that half. It opens the connection
with the sync packet a hood sends (nine bytes of `0x11`), keeps the state a
status frame carries, and answers every press with the frame a real hood would
have sent, transient lift movement included.

That makes the LEDs a readout of our own decoding: press Effektbeleuchtung, and
if the right lamp lights on the remote, the bit we set is the bit the remote
reads. The hood state it keeps follows `BerbelRemote/src/berbel_protocol.h`.

`run` keeps a control socket open (`/tmp/fakehood.sock` by default), so a
second shell can drive the session while it records:

```bash
python3 fakehood.py ctl label "BFB 7bT, top left button"
python3 fakehood.py ctl status 100000000000000000      # write a status frame
python3 fakehood.py ctl sync                           # write the 0x11 sync packet
python3 fakehood.py ctl push                           # re-send the emulated state
python3 fakehood.py ctl write button 0a00              # write on f004f002
python3 fakehood.py ctl read battery
python3 fakehood.py ctl info                           # discovered GATT
python3 fakehood.py ctl state                          # connection and hood state
python3 fakehood.py ctl profile profiles/bfb-7bt.json  # record everything about it
python3 fakehood.py ctl quit
```

`status` writes a frame by hand and is the tool for a bit sweep: set one bit,
look at the remote, move on. It bypasses the emulated state, so the next press
overwrites it.

`label` writes a line into the log, which is what turns a capture into a
button map: label the button, press it, move on.

`profile` dumps the whole peripheral to JSON, which is how the files in
`profiles/` were made. Do that once while the hardware is in front of you: the
file outlives the remote, and everything afterwards can be compared against it.

## Output

Every event lands in the JSONL log and on stdout. Button codes are resolved
against `berbel_button_map.json` when the file is found next to the script or
in a parent directory; unknown codes print raw, which is the interesting case.

```
14:02:11.417  BUTTON   0a 00        0x0A  light_down (Kochfeld-Beleuchtung)
14:02:11.583  BUTTON   00 00        release
14:02:19.006  BUTTON   14 00        0x14  UNKNOWN CODE
```

## When it does not work

- **The remote never shows up in a scan.** It only advertises while it has
  something to say. Press a button, or hold "1" + "P" to put it into pairing
  mode.
- **Pairing fails.** Turn Secure Connections off as above. A `--unpair` run
  drops a stale bond on the Linux side; the remote's own bond is cleared with
  the disconnect combination.
- **The connection drops during service discovery.** `failed to discover
  services, device disconnected` on the first attempt and fine on the retry is
  normal; the tool reconnects rather than giving up. A remote that was asleep
  also swallows the press that woke it.

  What is *not* the problem: the remote's MAC filtering. The hood filters
  remotes by OUI, so the reverse looked plausible, but a BFB 8bT and a BFB 7bT
  both bonded happily with a Linux central whose Intel address resembles
  neither a hood nor a remote. No spoofing needed.
- **Pairing needs to be inspected.** BlueZ does SMP in the kernel and shows
  nothing of the negotiation. An nRF52840 with the nRF Sniffer firmware
  follows the connection passively and shows the exchange in Wireshark.
