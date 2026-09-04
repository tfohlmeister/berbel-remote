# Reverse Engineering: Berbel BFB 6bT BLE Protocol

Documentation of the reverse-engineered BLE protocol used by Berbel kitchen hoods and the BFB 6bT remote control (Art. 1090045). All findings were obtained by sniffing BLE traffic between the original remote and hood using an nRF52840 dongle with Wireshark.

## Hardware

### Original Remote (BFB 6bT)
- **Chip:** Texas Instruments CC26xx series (BLE SoC)
- **MAC OUI:** `88:01:F9:xx:xx:xx` or `30:AF:7E:xx:xx:xx`
- **Role:** BLE Peripheral (advertises and waits for connection)

### Hood
- **Chip:** ESP32 (Espressif)
- **MAC OUI:** `84:F7:03:xx:xx:xx`
- **Role:** BLE Central (scans, initiates connections and pairing)

## MAC Address OUI Filtering

The hood filters BLE devices by MAC address OUI and **only accepts connections from Texas Instruments MACs**. Devices with other OUIs (e.g., Espressif) are silently ignored, even with correct advertising data and GATT services.

Accepted OUIs:
- `88:01:F9:xx:xx:xx`
- `30:AF:7E:xx:xx:xx`

ESP32 workaround (must be called before BLE init):
```cpp
uint8_t ti_mac[6] = {0x88, 0x01, 0xF9, 0xAA, 0xBB, 0xCC};
esp_base_mac_addr_set(ti_mac);
```

## Advertising

The hood is strict about the advertising packet structure. It matches specific custom service data, not HID or appearance fields.

- **Address Type:** Public
- **PDU Type:** `ADV_IND`
- **Flags:** `0x05` (Limited Discoverable, BR/EDR Not Supported)
- **Service Data (128-bit):** UUID `f000f000-5745-4053-8043-62657262656c`, Data `0x01` (ACTIVE)
- **Scan Response:** Empty (0 bytes)

Raw advertising data (22 bytes):
```
02 01 05                                         // Flags
12 21                                            // Service Data header (length 18, type 0x21)
6c 65 62 72 65 62 43 80 53 40 45 57 00 f0 00 f0 // UUID (little-endian)
01                                               // ACTIVE state
```

Do **not** include HID UUID, Local Name, or Appearance in the advertisement. Service Data value must be `0x01`, not `0x00`.

## Security & Pairing

| Property | Value |
|----------|-------|
| Pairing Type | Legacy Pairing (no Secure Connections) |
| IO Capability | No Input, No Output (Just Works) |
| Bonding | Required (LTK exchange) |
| Key Distribution | **LTK only** (no IRK, no CSRK) |
| Initiator | Hood (Central) sends Pairing Request |

Distributing IRK in addition to LTK may cause pairing failures.

NimBLE configuration:
```cpp
NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);
NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);
```

## GATT Services

Services must be created in this exact order. The hood validates the service structure.

### Service Overview

| Handle | UUID | Service |
|--------|------|---------|
| 0x0001 | 0x1800 | Generic Access (auto-created) |
| 0x0008 | 0x1801 | Generic Attribute (auto-created) |
| 0x000C | 0x180A | Device Information |
| 0x000F | 0x180F | Battery Service |
| 0x0014 | 0x1812 | HID Service |
| 0x0031 | f004f000-...-berbel | Berbel Custom Service |

HID and Battery services are **required**. The hood will connect but ignore the device if they are missing.

### Device Information (0x180A)

| Handle | UUID | Properties | Description |
|--------|------|------------|-------------|
| 0x000D | 0x2A50 | read | PnP ID |

### Battery Service (0x180F)

| Handle | UUID | Properties | Description |
|--------|------|------------|-------------|
| 0x0010 | 0x2A19 | read, notify | Battery Level |

### HID Service (0x1812)

The remote exposes a standard HID profile. Button commands are **not** sent via HID reports; the HID service is only required for the hood to accept the device.

| Handle | UUID | Properties | Description |
|--------|------|------------|-------------|
| 0x0016 | 0x2A4A | read | HID Information |
| 0x0018 | 0x2A4C | write-without-response | HID Control Point |
| 0x001A | 0x2A4E | read, write-without-response | Protocol Mode |
| 0x001C | 0x2A4B | read | Report Map |
| 0x001F | 0x2A4D | read, notify | Report (Input) |
| 0x0023 | 0x2A4D | read, notify | Report (Input) |
| 0x0027 | 0x2A4D | read, write-without-response, write | Report (Output/Feature) |
| 0x002A | 0x2A4D | read, notify | Report (Input) |
| 0x002E | 0x2A4D | read, write-without-response, write | Report (Output/Feature) |

### Berbel Custom Service (f004f000-...-berbel)

This is the proprietary service used for all communication between remote and hood.

| Handle | UUID | Properties | Direction | Description |
|--------|------|------------|-----------|-------------|
| 0x0032 | f004f001-...-berbel | read, write-without-response | Hood -> Remote | **Hood status** (9 or 13-byte state packets) |
| 0x0034 | f004f002-...-berbel | read, notify | Remote -> Hood | **Button commands** (2-byte notifications) |

UUID breakdown:
```
f004f000-5745-4053-8043-62657262656c
         "WE"  "P"  "C"   "berbel" (ASCII)
```

## Button Protocol

Commands are sent as 2-byte notifications on `f004f002`:
- **Press:** `[code, 0x00]`
- **Release:** `[0x00, 0x00]`

### Complete Button Mapping

Codes are reverse engineered by pressing each button and recording the notification. The function names come
from the Berbel manual (document 6005229_0); the manual does not publish the codes, so the mapping between the
two is only as good as the hood it was confirmed on. The manual's own function table lists Kochfeld-Beleuchtung
before Effektbeleuchtung, but on the hoods tested so far the codes are the other way round, so do not assume
the unverified rows follow the manual's order either.

| Code | Function | Berbel Manual Term | Verified |
|------|----------|--------------------|----------|
| 0x01 | Power | EIN/AUS | yes |
| 0x02 | Fan 1 | Leistungsstufe 1 | yes |
| 0x03 | Fan 2 | Leistungsstufe 2 | yes |
| 0x04 | Fan 3 | Leistungsstufe 3 | yes |
| 0x05 | Fan P | Leistungsstufe POWER | yes |
| 0x06 | Effect Light (Oberlicht) | Effektbeleuchtung | yes |
| 0x07 | Sync | Synchronisation | no, by elimination |
| 0x08 | Recirculation | Umluftbetrieb / Kontrollanzeige Filter | no, by elimination |
| 0x09 | Raise | Liftfunktion "Heben" | yes |
| 0x0A | Cooktop Light (Unterlicht) | Kochfeld-Beleuchtung | yes |
| 0x0B | Multifunction | Multifunktionstaste (assigned in the Berbel app, e.g. Deckenanschluss mit Effektbeleuchtung) | yes |
| 0x0C | Afterrun | Nachlauffunktion | yes |
| 0x0D | Lower | Liftfunktion "Senken" | yes |
| 0x12 | Ceiling Light | Deckenlicht; not a BFB 6bT button | yes, on a Skyline Edge Play |

## The berbel App as a Second Source

The Skyline Edge hoods expose a GATT server of their own, which the berbel
iPhone app talks to. Captured with PacketLogger by @jens-42 on a Skyline Edge
Play ([issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3)):

| App action | Characteristic | Value |
|---|---|---|
| Lüfter Stufe 1 | `f006f004` | `0002` |
| Lüfter Stufe 2 | `f006f004` | `0003` |
| Lüfter Stufe 3 | `f006f004` | `0004` |
| Lüfter Stufe P | `f006f004` | `0005` |
| Kochfeld-Beleuchtung toggle | `f006f004` | `000a` |
| Nachlauffunktion | `f006f004` | `000c` |
| Deckenlicht toggle | `f006f004` | `0012` |
| Effektbeleuchtung on | `f006f006` | `00a30000ff00000005ffffff00ff0000` + 15 zero bytes |
| Effektbeleuchtung off | `f006f006` | `00a30000ff00000005ffffff0000…` |

**Those are our button codes.** `0002` through `0005`, `000a` and `000c` carry
the same code numbers this firmware sends to `f004f002` as a remote. The framing
differs: the app writes the code in the second byte (`00 02`), the remote sends
it in the first (`02 00`, followed by `00 00` as the release). Same vocabulary,
two channels, so the app is a legitimate source for codes the remote's manual
does not name. `0012` for the Deckenlicht was found precisely this way and then
confirmed on the hood.

### `f006f006`, the colour channel: handle with care

Colour and brightness are written to `f006f006` as 31-byte payloads rather than
button codes. This firmware does not speak it.

There is a report of it destabilising a hood. @jens-42 wrote that controlling a
Skyline Edge Play over `f006f006` "frequently leads to crashes, requiring me to
flip the circuit breaker to restart the unit", with gesture control (the laser
dot) dead until then, while the two-byte writes to `f006f004` were "absolutely
reliable" for him. That is one report from one hood, not reproduced here, and it
may well depend on the model or on how the writes are paced. Worth knowing
rather than worth panicking about.

None of it affects using this firmware, which never writes to `f006f006`. It is
something to keep in mind if you extend it towards colour, since that is the
channel you would need.

[berbel-ha](https://github.com/dirkbloessl/berbel-ha) sends its light and fan
commands over `f006f006` (`WRITE_COMMANDS` in its `const.py`) and evidently works
for its users. Its status parsing is independently useful and agrees with ours.

## Hood Status Protocol

The hood writes bitmask-based status packets to `f004f001`. **The frame length
differs by model**, and so does the position of at least one flag, so the
firmware picks a layout table by frame length (`berbel_protocol.h`).

### 9-byte frame (BFB 6bT)

| Byte | Mask | Meaning |
|------|------|---------|
| [0] | 0x10 | Fan Stufe 1 |
| [1] | 0x01 | Fan Stufe 2 |
| [1] | 0x10 | Fan Stufe 3 |
| [2] | 0x09 | Fan Power |
| [2] | 0x10 | Oberlicht (upper light) |
| [4] | 0x10 | Unterlicht (cooktop light) |
| [4] | 0x01 | Cover moving up (retracting) |
| [5] | 0x01 | Deckenlicht (ceiling connection light) |
| [5] | 0x90 | Nachlauf (afterrun timer active) |
| [6] | 0x01 | Cover moving down (deploying) |

Bytes [3], [7] and [8] are `0x00` in every frame observed so far, with one
exception described below. Nothing decodes them.

### 13-byte frame (Skyline Edge Play)

Measured in [issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3).
The fan steps and the Unterlicht keep their positions; the Deckenlicht moves
from byte [5] to byte [9].

| Byte | Mask | Meaning | Confirmed on a 13-byte hood |
|------|------|---------|------------------------------|
| [0] | 0x10 | Fan Stufe 1 | yes, issue #3 and berbel-ha |
| [1] | 0x01 | Fan Stufe 2 | yes, issue #3 and berbel-ha |
| [1] | 0x10 | Fan Stufe 3 | yes, issue #3 and berbel-ha |
| [2] | 0x09 | Fan Power | **no, carried over from the 9-byte layout** |
| [2] | 0x10 | Oberlicht (upper light) | yes, berbel-ha |
| [4] | 0x10 | Unterlicht (cooktop light) | yes, issue #3 and berbel-ha |
| [4] | 0x01 | Cover moving up (retracting) | yes, issue #3 |
| [9] | 0x01 | Deckenlicht (ceiling connection light) | yes, issue #3 |
| [5] | 0x90 | Nachlauf (afterrun timer active) | yes, berbel-ha |
| [6] | 0x01 | Cover moving down (deploying) | yes, issue #3 |

Fan Power is the only field still taken from the 9-byte layout without a
measurement to back it. [berbel-ha](https://github.com/dirkbloessl/berbel-ha)
(MIT), an independent Home Assistant integration for the Skyline Edge Base that
reverse engineered the same hoods through a different channel, has it as
`FAN_LEVEL_4 = 0x19  # Unknown (no level 4 available)`, so nobody has pinned it
down. Everything else in its `parser.py` agrees byte for byte and mask for mask
with the table above, which is where the "berbel-ha" confirmations come from.

In the frames measured so far, bytes [3], [7], [8] and the four extra bytes
[10] to [12] were always `0x00`. Whether byte [3] behaves on this model as it
does on the short frame (see below) has not been observed.

**Open question:** why the two layouts exist, and how the original remote tells
them apart. Its buttons light up in sync with the hood, so it decodes the same
frames and must be making the same distinction, whether by frame length, by a
model identifier exchanged during pairing, or by shipping firmware per model.
Anything measured on a third model is welcome, particularly the flags marked
"carried over" above, which are assumed rather than confirmed.

### Byte [3]: seen but not understood

`byte[3]` takes the value `0xD0` during one specific phase and is `0x00` at
every other moment, including all button-driven cover moves, every fan level,
both lights and the afterrun. The phase is the automatic descent a lift model
performs when the fan is switched on from the parked position, and even there
the byte does not stay set: it appears, clears, and appears again while the
hood is moving.

What it means is unknown. It is not a position: the value never varies with how
far the hood has travelled, and it is absent from the button-driven moves that
cover the same distance. Nor is it a direction, since it never accompanies a
retraction. It behaves more like a transient mode or status marker for the
"deploy before running" sequence than like a field with a value.

The firmware ignores it. It is recorded here because it is the only byte in the
frame that is known to carry information we cannot read, and because a hood
that reports an absolute position would most plausibly do it here.

### Cover movement on the lift models

A button-driven move sets exactly one of the two movement flags. Starting the
fan from the parked position is different: the hood drives down first, and for
the roughly ten seconds that takes, byte [4] and byte [6] carry the **same**
value, first `0x0D` and then `0x01`. Both flags set therefore means deploying,
never retracting, and the decoder gives byte [6] priority for that reason.

Measured on a BFB 6bT with lift, fan switched to Stufe 1 from the parked
position:

```
10 00 00 D0 00 00 00 00 00
10 00 00 D0 0D 00 0D 00 00
10 00 00 00 0D 00 0D 00 00
10 00 00 00 01 00 01 00 00
10 00 00 D0 01 00 01 00 00
10 00 00 00 00 00 00 00 00   <- ten seconds later, hood is down
```

Note the `0xD0` in `byte[3]` above, which is the only place it ever shows up.

No frame carries an absolute position, so "up" and "down" are inferred from the
direction of the last movement rather than read from the hood. Switching the
hood off produces no movement frames at all: it does not retract on its own.

On connect, the hood sends a sync packet (all bytes `0x11`) which should be ignored.

## Wireshark Analysis Commands

Useful tshark filters for analyzing BLE captures with an nRF52840 sniffer.

**Connection flow** (confirm hood initiates connection):
```bash
tshark -r your-capture.pcapng \
  -Y "btle.advertising_header.pdu_type == 0x05" \
  -T fields -e frame.time -e btle.initiator_address -e btle.advertising_address
```

**Security/pairing exchange:**
```bash
tshark -r your-capture.pcapng -Y "btsmp"
```

**GATT notifications** (button commands on custom characteristic):
```bash
tshark -r your-capture.pcapng \
  -Y "btatt.opcode == 0x1b" \
  -T fields -e btatt.handle -e btatt.value -e btatt.uuid128
```

**Advertisement data inspection:**
```bash
tshark -r your-capture.pcapng \
  -Y "btle.advertising_address == <remote-mac> && btle.advertising_header.pdu_type == 0x00" -V
```
