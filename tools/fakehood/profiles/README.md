# Remote profiles

What a BLE central sees of a real Berbel remote, recorded while the hardware was
on the bench. The remotes themselves were borrowed and have gone back, so these
files are the only remaining reference.

| File | Remote | Status frame | Device name | MAC OUI |
|---|---|---|---|---|
| `bfb-7bt.json` | BFB 7bT (Art. 1090084) | 13 bytes | `Hood BLE v7` | `0C:0A:DF` |
| `bfb-8bt.json` | BFB 8bT (Art. 1090093) | 9 bytes | `Hood BLE v6.1` | `FC:DE:C5` |

Each file holds the full GATT tree with every readable value, the raw
advertising PDUs in both states, and the negotiated frame length.

## What they are for

The firmware emulates a remote. These files say what it is emulating, in the
only terms that matter: what a central actually observes. Diff an emulator
against one of them and every difference is a candidate reason a hood might
treat it differently. That comparison needs no hardware, which is the point.

## Reading them

Two fields describe the advertisement and they are not equally trustworthy.

- **`raw_advertising`** is the actual PDU, captured with `btmon`. Use this.
- **`advertisement_as_bluez_saw_it`** is what a scanner reported. BlueZ fills
  its `Name` and `UUIDs` properties from the GATT database as well as from the
  advertisement, so this block shows a name and a service list that a real
  advertisement does not contain. It is kept because it is what any bleak-based
  tool will see, and mistaking it for the PDU is an easy error to make.

The 8bT profile illustrates it: `advertisement_as_bluez_saw_it.local_name` says
`Hood BLE v6.1`, while the raw capture shows that remote sends no scan response
at all and never advertises a name.

One value is not the remote's own: `f004f001` in the 8bT profile reads back a
status frame the fake hood had written during the session, not an initial value.

## How they were recorded

```bash
python3 fakehood.py --log capture.jsonl run --emulate
python3 fakehood.py ctl profile profiles/bfb-7bt.json
```

The raw advertising was captured separately, since BlueZ does not expose it:

```bash
sudo btmon -w adv.snoop &
sudo bluetoothctl --timeout 120 scan on
# press a key on the remote, then hold 1 + P for 3 s
sudo btmon -r adv.snoop
```
