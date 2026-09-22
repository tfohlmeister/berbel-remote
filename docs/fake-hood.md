# Reading a remote out with a fake hood

Everything in this repository so far was learned by watching a remote talk to
a hood, either with a sniffer or by standing in for the remote. That leaves
one side unreachable: a remote nobody owns a matching hood for, and any button
whose function the hood does not perform.

The fake hood closes that gap. In the Berbel protocol the remote is the BLE
peripheral and the hood is the central, so a Linux box with a BLE adapter can
play the hood: find the remote, bond with it, subscribe to the button
characteristic and record what it sends. The tool is
[`tools/fakehood/`](../tools/fakehood/), which also documents how to run it.

The remote answers in both directions, which is what makes this more than a
code dump:

- **Buttons out.** Every press arrives as a notification on `f004f002`,
  whether or not any hood would act on it. Codes that no hood in reach
  performs are readable this way.
- **Status in.** `f004f001` accepts the same status frames a hood writes, and
  the remote mirrors them on its LED. A frame can therefore be checked against
  what the hardware shows rather than against another capture.

## What came out of it

Two remotes were read out this way, a **BFB 8bT** and a **BFB 7bT**, neither of
which had ever been paired to a hood.

- **All 19 codes of the BFB 7bT**, six of them new to this project: `0x0E`,
  `0x13`, `0x14`, `0x15`, `0x16` and `0x17`. They cover colour temperature per
  lamp, both Motion-Lights keys, and the key the manual itself lists as *ohne
  Funktion*, which turns out to send `0x0E` regardless.
- **All 13 codes of the BFB 8bT**, which are exactly the 6bT's `0x01` to `0x0D`.
  That had been an assumption in this repository and is now measured.
- **`0x07` and `0x08` are no longer guesses.** They were assigned by elimination
  because no hood here performs Synchronisation or Umluft. Read off a remote and
  matched to its manual's symbol table, they turn out to have been right.
- **A long press carries no separate code.** The remote sends the ordinary press
  and delays the release, so the hood times the key, not the remote.
- **The status frame length belongs to the remote.** The 8bT sizes its status
  characteristic at 9 bytes, the 7bT at 13, before either has seen a hood.
- **A third and fourth Texas Instruments OUI**, `FC:DE:C5` and `0C:0A:DF`.
- **Fan Power in the 13-byte frame is `byte[2] & 0x01`**, the last field in that
  layout that nobody had measured. The old `0x09` mask turns out to be two
  separate bits: `0x01` lights the P key steadily, `0x08` alone does nothing,
  and both together make it blink. What `0x08` means is still unknown.
- **The advertisement carries a pairing-mode flag, not an "active" flag.** A
  real remote sends `0x01` in the service data only while in pairing mode and
  `0x00` once bonded, uses General rather than Limited Discoverable flags, and
  answers a scan request with its name while pairing.

The codes live in [`berbel_button_map.json`](../berbel_button_map.json) and the
full table with sourcing is in
[REVERSE_ENGINEERING.md](../REVERSE_ENGINEERING.md).

### How the codes were tied to the buttons

Pressing 19 keys gives 19 codes in order, which is only a mapping if the order
is trustworthy. The method: press every key once in reading order, then align
the recording against the layout.

The alignment proves itself. Of the 7bT's nineteen keys, eight carry codes this
project already knew, and those eight act as anchors: with the recording shifted
into place, `P` lands on `0x05`, `3` on `0x04`, `2` on `0x03`, `1` on `0x02`,
Umluft on `0x08`, Heben on `0x09`, Nachlauf on `0x0C` and Senken on `0x0D`. Eight
independent hits at once. Any other offset breaks all of them, which is how the
one press lost to a sleeping remote at the start of the walk was found.

The same argument fixes the eight lighting keys, which is where the new codes
are. Their names come from the manual's symbol table, read in the same reading
order, and **two of the eight anchors sit inside that group**: Effektbeleuchtung
on `0x06` and Kochfeld-Beleuchtung on `0x0A`, both landing exactly where that
reading predicts. A column-wise table order, or swapped pairs within a row,
would put them somewhere else.

## What the hardware says about itself

Read off a remote the box labelled **BFB 8bT**, firmware `1.0.57`:

| Characteristic | Value |
|---|---|
| Manufacturer Name (`0x2A29`) | `berbel` |
| Model Number (`0x2A24`) | `xFB7` |
| Serial Number (`0x2A25`) | `FC:DE:C5:B0:25:44` (its own MAC) |
| Firmware Revision (`0x2A26`) | `1.0.57` |
| Hardware Revision (`0x2A27`) | `0x002E0401` |
| Software Revision (`0x2A28`) | `6.4.137` |
| System ID (`0x2A23`) | `FC:DE:C5` + `FF:FE` + `B0:25:44` |
| PnP ID (`0x2A50`) | USB-IF, vendor `0x119B`, product `0x2129`, revision `0x0001` |

Two things follow from that.

**`FC:DE:C5` is a third Texas Instruments OUI.** The README lists `88:01:F9`
and `30:AF:7E` as the prefixes a hood accepts, which is what two remotes
happened to carry. A remote in the wild uses a TI prefix that is on neither
list, so the hood's rule is "a TI OUI", and the two known values are a sample
rather than the set. Anything that spoofs a MAC to satisfy a hood should be
read with that in mind.

**The remote is a ruwido OEM product.** Both the USB vendor ID `0x119B` and
the member service UUID `0xFE0B` below are registered to ruwido austria gmbh,
a manufacturer of BLE remote controls. berbel specifies the remote; ruwido
builds it on TI radio.

## The GATT server the remote exposes

Discovered on the same device. The Berbel service matches what the firmware
emulates; everything around it is richer than the emulation needs to be.

| Service | Characteristic | Properties |
|---|---|---|
| Generic Access `0x1800` | `0x2A00` Device Name, `0x2A01` Appearance, `0x2A04` Preferred Connection Parameters, `0x2AC9` Resolvable Private Address Only | read |
| Generic Attribute `0x1801` | `0x2A05` Service Changed | indicate |
| Device Information `0x180A` | `0x2A23` … `0x2A29`, `0x2A50` | read |
| Battery `0x180F` | `0x2A19` Battery Level | read, notify |
| HID `0x1812` | `0x2A4A` HID Information, `0x2A4B` Report Map, `0x2A4C` Control Point, `0x2A4E` Protocol Mode, four `0x2A4D` Reports | mixed |
| **Berbel** `f004f000-…` | `f004f001` status, hood to remote | read, write-without-response |
| | `f004f002` buttons, remote to hood | read, notify |
| ruwido `0xFE0B` | `00000002-bebc-4ca4-a6f8-bb896399027f` | write, write-without-response, notify |

The `0xFE0B` service is not part of the Berbel protocol. Its UUID is
registered to ruwido, and its single characteristic takes writes and sends
notifications. What it carries is unknown; an OTA or configuration channel
would fit the shape, but nothing here has probed it.

## What was confirmed on real hardware

- **Pairing needs no adapter preparation.** BlueZ negotiates down to legacy
  pairing on its own, and a `NoInputNoOutput` agent registered by the tool
  completes Just Works unattended. Secure Connections never had to be turned
  off.
- **The remote does not filter the central's MAC.** It bonded with an Intel
  adapter whose address has no relation to the hood's Espressif OUI. The OUI
  filtering documented in this repository is the hood's behaviour and has no
  counterpart on the remote.
- **`f004f001` is a nine-byte buffer.** It reads back as nine zero bytes
  before anything is written, and a written frame reads back unchanged.
- **The first connection attempt can fail** with "failed to discover services,
  device disconnected" and succeed on the immediate retry, which is why the
  tool reconnects rather than giving up.
- **A remote asleep at the start of a walk swallows the first press.** It wakes,
  advertises and reconnects on it, so the press that woke it is not recorded.
  Press anything once before starting to count.
- **Timing through this channel is quantised to 45 ms.** Every press duration
  measured across both remotes is a multiple of it, and the 7bT's Peripheral
  Preferred Connection Parameters name 45 ms as its maximum interval. Both
  notifications of a press ride connection events, so nothing finer is visible,
  and nothing finer is available to a hood either.

## Reading the status layout off the remote's LEDs

A remote lights its own keys from the frames the hood writes, so `--emulate`
turns it into a readout of our own decoding: write a bit, look at the remote,
write the next.

```bash
python3 fakehood.py ctl status 00000100000000000000000000   # byte[2] = 0x01
```

This is how Fan Power in the 13-byte layout was finally pinned down. Two things
to know before trying it.

**The camera cannot do this for you.** The bench webcam detected no change at
all when a key lit in daylight; these LEDs are small and dim. Every reading here
was taken by eye.

**A sweep is a bad idea.** Stepping through all 13 bytes on a timer and asking
someone to count unlabelled flashes does not work in practice. Ask one question
at a time: write a state, ask what lights, write the next. Four targeted writes
settled Fan Power, where a full sweep would have been 104.

And remember what is being measured: how a *remote* interprets a frame. That is
the same decoding job this firmware does, which is what makes it useful, but it
is not proof of what a *hood* puts on the wire.

## Method

Put the remote into pairing mode ("1" + "P" for more than 3 seconds), start
`fakehood.py run`, and it bonds on its own. From there, a button walk is a
label and a press:

```bash
python3 fakehood.py ctl label "row 2, middle: burst icon"
# press the button
```

Both land in the JSONL log in order, which turns the capture into a mapping
without anyone having to remember what was pressed when.
