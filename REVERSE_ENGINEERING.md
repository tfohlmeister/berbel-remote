# Reverse Engineering: the Berbel BLE Protocol

Documentation of the reverse-engineered BLE protocol used by Berbel kitchen hoods and their remote
controls: the **BFB 6bT** (Art. 1090045), the **BFB 8bT** (Art. 1090093) that replaced it, and the
**BFB 7bT** (Art. 1090084) that ships with the Skyline Edge models.

Two routes produced what is here. The hood-side findings come from sniffing BLE traffic between an
original remote and a hood with an nRF52840 dongle and Wireshark. The remote-side findings, which is
every code the 8bT and 7bT send, come from the [fake hood](docs/fake-hood.md): a Linux BLE central
that a remote pairs with and talks to, so a remote can be read out with no hood involved at all.

## Hardware

### Original Remote (BFB 6bT)
- **Chip:** Texas Instruments CC26xx series (BLE SoC)
- **MAC OUI:** `88:01:F9:xx:xx:xx` or `30:AF:7E:xx:xx:xx`
- **Role:** BLE Peripheral (advertises and waits for connection)

### BFB 8bT
- **MAC OUI:** `FC:DE:C5:xx:xx:xx`, also Texas Instruments
- **Built by:** ruwido austria gmbh. Its USB vendor ID `0x119B` is in the remote's PnP ID, and the
  extra service `0xFE0B` it exposes is ruwido's registered member UUID.
- **Device name:** `Hood BLE v6.1`, readable over GATT but never advertised
- **Status frame:** 9 bytes
- **Role:** BLE Peripheral, same as the 6bT

### BFB 7bT
- **MAC OUI:** `0C:0A:DF:xx:xx:xx`, Texas Instruments again
- **Device name:** `Hood BLE v7`, advertised in a scan response while pairing
- **Status frame:** 13 bytes
- **Role:** BLE Peripheral

Both were read out directly with the [fake hood](docs/fake-hood.md). Their complete GATT trees, every
readable value and the raw advertising PDUs are recorded in
[`tools/fakehood/profiles/`](tools/fakehood/profiles/); the remotes themselves were borrowed and have
gone back, so those files are the reference now.

**The device name encodes the generation.** The 8bT calls itself `Hood BLE v6.1` and speaks the 6bT's
codes; the 7bT calls itself `Hood BLE v7` and adds seven. Of everything a hood can see, this is the
cleanest discriminator, and the only field where the two differ in a way that looks deliberate rather
than incidental. Whether a hood reads it is unknown.

Beyond the name, the two are the same product with different populations: identical GATT structure,
identical model number `xFB7`, identical firmware revision `1.0.57` and identical PnP ID. They differ
in hardware revision (`0x002E0401` against `0x002E0201`), software revision (`6.4.137` against
`6.4.68`), status frame length and MAC prefix.

**The frame length belongs to the remote, not to the hood.** The 13-byte layout was known only from a
Skyline Edge Play, so it was reasonable to read it as a property of that hood. A BFB 7bT that has never
seen a hood sizes its own status characteristic at 13 bytes and an 8bT at 9, which settles it: the
remote decides, and a hood writing to one has to match what it finds.

### Hood
- **Chip:** ESP32 (Espressif)
- **MAC OUI:** `84:F7:03:xx:xx:xx`
- **Role:** BLE Central (scans, initiates connections and pairing)

## MAC Address OUI Filtering

The hood filters BLE devices by MAC address OUI and **only accepts connections from Texas Instruments MACs**. Devices with other OUIs (e.g., Espressif) are silently ignored, even with correct advertising data and GATT services.

Known-good OUIs, each taken from a remote that works:
- `88:01:F9:xx:xx:xx`
- `30:AF:7E:xx:xx:xx`
- `FC:DE:C5:xx:xx:xx` (BFB 8bT)
- `0C:0A:DF:xx:xx:xx` (BFB 7bT)

That is a sample, not the rule. Four remotes carried four different Texas Instruments prefixes, so the
hood tests for the manufacturer rather than for a list of values, and a fifth remote would plausibly
add a fifth prefix. Spoofing any of the four is what has been shown to work.

The filter is the hood's behaviour and has no counterpart on the remote: a BFB 8bT bonded happily
with a Linux central whose Intel address resembles neither a hood nor a remote.

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

Raw advertising data (22 bytes), as this firmware sends it:
```
02 01 05                                         // Flags
12 21                                            // Service Data header (length 18, type 0x21)
6c 65 62 72 65 62 43 80 53 40 45 57 00 f0 00 f0 // UUID (little-endian)
01                                               // ACTIVE state
```

Do **not** include HID UUID, Local Name, or Appearance in the advertisement.

### What a real remote actually sends

Captured off a BFB 7bT and a BFB 8bT with `btmon`, which is the only way to see
the raw PDU: BlueZ fills its own `Name` and `UUIDs` properties from the GATT
database as well, so a scanner's view of an advertisement cannot be trusted for
this, and will show a name and a service list the air never carried.

The structure above is right, and 22 bytes of flags plus service data is
exactly what comes out: no name, no HID UUID, no appearance in the `ADV_IND`.
Three details differ, and the two remotes agree on all three.

| | This firmware | BFB 7bT and 8bT |
|---|---|---|
| Flags | `0x05`, LE **Limited** Discoverable | `0x06`, LE **General** Discoverable, in both states |
| Service data value | always `0x01` | `0x01` while in pairing mode, `0x00` otherwise |
| Scan response | empty | 7bT sends its name while pairing, 8bT sends none at all |

Normal operation, advertising to reconnect to a hood it is already bonded to:
```
02 01 06 12 21 6c 65 62 72 65 62 43 80 53 40 45 57 00 f0 00 f0 00
```

Pairing mode, after holding "1" + "P" for more than 3 seconds, LED blinking:
```
ADV_IND:  02 01 06 12 21 6c 65 62 72 65 62 43 80 53 40 45 57 00 f0 00 f0 01
SCAN_RSP: 0c 09 48 6f 6f 64 20 42 4c 45 20 76 37  04 0d 0c 05 20
          └ Complete Local Name "Hood BLE v7"    └ Class of Device 0x20050C
```

So the service data byte is not an "active" flag but a pairing-mode flag, and a
real remote leaves that state once it is bonded. This firmware stays in it.

**None of this is a known problem.** The firmware is in production use with
these three deviations and reaches its hoods, so they are evidently tolerated.
It is recorded because it is measured, not because anything is broken; anyone
changing the advertisement to match should test it against their own hood
first, since the current behaviour is the one with a track record.

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
from the Berbel manual; the manual does not publish the codes, so the two had to be joined by hand.

Every row below was measured. `0x01` to `0x06` and `0x09` to `0x0D` were driven on a hood and watched; that
is what "on a hood" means in the table. The remaining two, `0x07` and `0x08`, were long assigned by
elimination only, because no hood in reach performs either function. They are now measured directly on a
BFB 8bT with the [fake hood](docs/fake-hood.md), which reads a remote without needing a hood at all, and
matched against the symbol table in the remote's own manual (document 6007335_0), which prints the symbol
next to each function name and so binds a button to a function without any guessing. Both rows turned out
to have been guessed correctly. That symbol table also lists Effektbeleuchtung before Kochfeld-Beleuchtung,
the same order the codes run in, which removes the reason this section used to give for distrusting the
order. The 6bT manual (6005229_0) is the one reported to list the two lights the other way round; nothing
here re-checked that.

| Code | Function | Berbel Manual Term | On which remote | Verified |
|------|----------|--------------------|-----------------|----------|
| 0x01 | Power | EIN/AUS | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x02 | Fan 1 | Leistungsstufe 1 | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x03 | Fan 2 | Leistungsstufe 2 | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x04 | Fan 3 | Leistungsstufe 3 | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x05 | Fan P | Leistungsstufe POWER | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x06 | Effect Light (Oberlicht) | Effektbeleuchtung | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x07 | Sync | Synchronisation | 6bT, 8bT | 8bT |
| 0x08 | Recirculation | Umluftbetrieb / Kontrollanzeige Filter-Füllung | 6bT, 8bT, 7bT | 8bT, 7bT |
| 0x09 | Raise | Liftfunktion "Heben" | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x0A | Cooktop Light (Unterlicht) | Kochfeld-Beleuchtung | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x0B | Multifunction / Favourite scene | Multifunktionstaste (6bT, 8bT) · Favoritenszene (7bT) | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x0C | Afterrun | Nachlauffunktion (6bT, 8bT) · Leistungsstufe 0 / Nachlauffunktion (7bT) | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x0D | Lower | Liftfunktion "Senken" | 6bT, 8bT, 7bT | hood, 8bT, 7bT |
| 0x0E | "A" | ohne Funktion | 7bT | 7bT |
| 0x12 | Uplight | Uplight (nur BIH SKEP) | 7bT | Skyline Edge Play, 7bT |
| 0x13 | Uplight colour temperature | Farbtemperatur Uplight (nur BIH SKEP) | 7bT | 7bT |
| 0x14 | Cooktop light colour temperature | Farbtemperatur der Kochfeld-Beleuchtung | 7bT | 7bT |
| 0x15 | Effect light colour temperature | Farbtemperatur der Effektbeleuchtung | 7bT | 7bT |
| 0x16 | Motion-Lights on/off | Motion-Lights ein-/ausschalten (nur BIH SKEP) | 7bT | 7bT |
| 0x17 | Motion-Lights next | Motion-Lights durch-/weiterschalten (nur BIH SKEP) | 7bT | 7bT |

"hood" means the code was driven at a hood and the hood acted. "8bT" and "7bT" mean the remote emitted
that code when the button carrying that symbol was pressed, read with the [fake hood](docs/fake-hood.md).

The **BFB 8bT**'s thirteen buttons cover `0x01` to `0x0D` exactly, nothing left over and nothing missing,
so the 6bT and the 8bT speak the same vocabulary and the same firmware reaches both.

The **BFB 7bT**'s nineteen buttons cover `0x01` to `0x0E` and `0x12` to `0x17`, with `0x07` missing
because it has no Synchronisation key. `0x0F` to `0x11` were emitted by no button on either remote.

Three entries deserve a note.

- **`0x0B` is one code with two names.** The 8bT prints it as the Multifunktionstaste, the 7bT as the
  Favoritenszene. The remote sends the same byte either way; what it means is the hood's business. Any
  firmware exposing it should avoid naming it after one model's label.
- **`0x12` is the Uplight**, which is what the 7bT's own manual calls that key. This repository had it
  as "Deckenlicht" from [issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3), where it was
  found through the berbel app and confirmed on a Skyline Edge Play. Both descriptions point at the same
  lamp on the same hood; the manual's word is simply the better sourced one.
- **`0x0E` sits on the key the 7bT's manual lists as "ohne Funktion".** It has no function, and it still
  sends a code. A hood that does something with `0x0E` is therefore not out of the question.

### A long press is the same code, held

The remote has no separate code for a long press. Holding a button sends the ordinary
`[code, 0x00]` and simply delays the `[0x00, 0x00]` release, so **the hood measures the
duration, not the remote**. Measured on a BFB 7bT, on the two keys whose manual documents a
long press (Kochfeld-Beleuchtung as a dimmer, Umluft to restart the filter hour counter):

| Key | Code | Held |
|---|---|---|
| Kochfeld-Beleuchtung | `0x0A` | 2926 ms, 3420 ms |
| Umluftbetrieb | `0x08` | 360 ms, 585 ms, 3915 ms |
| Farbtemperatur Kochfeld | `0x14` | 270 ms, 270 ms, 271 ms |

That is also why emulating a long press needs nothing new: send the press, wait, send the
release. `berbel/hood/debug/send` with a hold time already does exactly this.

### Timing resolution: the 45 ms connection interval

Every press duration measured through this channel is a multiple of **45 ms**, which is
the resolution a hood can distinguish at all. Two independent lines of evidence:

- The largest interval that divides all 66 measured hold times across both remotes, within
  3 ms of logging jitter, is exactly 45.00 ms. Observed multiples run 1, 4, 5, 6, 7, 8, 13,
  65, 76, 87.
- The 7bT's Peripheral Preferred Connection Parameters (`0x2A04`) read
  `18 00 24 00 0a 00 c8 00`: minimum interval 24 units (30 ms), **maximum 36 units
  (45 ms)**, slave latency 10, supervision timeout 2000 ms.

The connection settles at the remote's preferred maximum, and both notifications of a press
ride connection events, so the gap between them can only ever be a multiple of it.

## The berbel App as a Second Source

The Skyline hoods expose a GATT server of their own, which the berbel app talks
to. Two independent looks at it exist.

**A capture.** PacketLogger on a Skyline Edge Play, by @jens-42
([issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3)):

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

**The app itself.** `de.berbel.plus` 1.2.0 for Android is a Flutter app whose
Dart AOT snapshot is not obfuscated, so class names, method names and the
constant pool survive decompilation. Everything below is read out of its machine
code, not measured on a hood.

### Two protocols, not one

`f004` and `f006` are separate worlds inside the same appliance, and they invert
the BLE roles:

| | `f004…` | `f006…` |
|---|---|---|
| GATT server lives on | the remote | the hood |
| The other side connects as | hood as central | app as central |
| Status framing | bitmasks over 9 or 13 bytes | one byte per value |
| What this firmware is | the server, a remote | nothing, it never connects |

So the app cannot explain the remote protocol's status frames, and reading it
tells us nothing new about them. For the button codes it is a genuine second
source, because both channels carry the same vocabulary.

### The button codes, confirmed by the vendor

The app's `RemoteControlButton` enum sits in the constant pool with both the
wire code and the vendor's own name for it. Twelve of our codes come back
unchanged:

| Code | Vendor name | Our label |
|---|---|---|
| 0x01 | `power` | power |
| 0x02 | `fanLevel1` | fan_1 |
| 0x03 | `fanLevel2` | fan_2 |
| 0x04 | `fanLevel3` | fan_3 |
| 0x05 | `p` | fan_p |
| 0x06 | `effectlight` | light_up |
| 0x09 | `arrowUp` | move_up |
| 0x0A | `cooklight` | light_down |
| 0x0B | `special` | multi |
| 0x0C | `postrun` | timer |
| 0x0D | `arrowDown` | move_down |
| 0x12 | `uplight` | light_uplight |

`pressRemoteControlButton` writes `00 <code>` and then `00 00` as the release,
the same two-frame gesture a remote makes, with the two bytes the other way
round.

**`0x07` and `0x08` do not appear in the app at all.** Both were measured on a
BFB 8bT with `tools/fakehood`, so this is not a gap in our reading; the app
simply has no model for a Synchronisation or a recirculation key, which are hood
equipment rather than app functions.

### `f006f006` is state, not a command channel

The app reads this characteristic, changes single bytes and writes the whole
array back. That is why the payloads look like frames with a fixed header: the
bytes nobody touched are simply what the hood last reported.

| Byte | Meaning |
|---|---|
| 0 | lift control: 0 standby, 1 hard stop, 2 stop, 3 up, 4 down, 5 slow up, 6 slow down |
| 1 | lift position, raw, no documented unit |
| 4 / 5 / 6 | brightness of cooklight / effectlight / ambilight, 0-255 |
| 7 | effect light mode: 0 warm white, 1 RGB, 2 light sequence |
| 9 / 10 / 11 | effect light red / green / blue |
| 13 | effect light brightness, 0-255 |
| 16 | fan speed as a number: 0 off, 1-3, 4 power, 5 P fix |
| 22 | filter saturation, 0-255 |
| 28 | active scene, 0 for none |

Bytes 2, 3, 8, 12, 14, 15, 17 to 21 and 23 to 27 are either not read by the app
or read without any consumer. Every write outside scene handling clears byte 28,
which is how changing anything by hand drops the active scene.

That decodes the capture above completely:

```
index  0  1  2  3  4  5  6  7  8  9 10 11 12 13 …
       00 a3 00 00 ff 00 00 00 05 ff ff ff 00 ff
                                  ^^ ^^ ^^    ^^
                                  r  g  b     brightness
```

White at full brightness, and the "off" variant differs in exactly byte 13.

The postrun is **not** in this frame. The app keeps a `postRunStartTimeStamp` of
its own and works it out locally.

### The other characteristics

| UUID | Direction | Content |
|---|---|---|
| `f006f001` | read | ASCII device type: model name, then `_100`/`_120`, `_BASE`/`_PLAY`, `_BUR`/`_BUF` |
| `f006f002` | read, write | the user-assigned device name, 30 bytes |
| `f006f007` | read, write | user settings, including colour temperature at byte 6/7/8 for cooklight/effectlight/ambilight |
| `f007f001`…`f007f016` | read, write, notify | 16 scene slots of 38 bytes each, name in 0-19 |

The slice boundaries in the device type string are read as 0-19, 20-23, 24-31
and 32-35, for 36 bytes in total. That rests on how a constant is tagged in the
decompiled code, and the alternative reading halves every offset. The token
lengths only fit the first reading, but a single read off a real hood would
settle it and nobody has done that.

The app finds hoods by manufacturer data alone: the first two bytes must be
`0c 90`. There is no service filter and no name prefix.

### Brightness and colour are a Play feature

On a `base` hood the app does not use any of this for the effect light. It falls
back to `pressRemoteControlButton(effectlight)`, the same button code this
firmware already sends. The dimming, RGB and colour temperature paths run only
for `play`. Anyone hoping to gain dimmable lights from `f006` should read the
variant out of `f006f001` first.

### Why this firmware does not speak it

Reaching `f006` would mean being a central as well as a peripheral, holding a
second connection in the opposite direction, and bonding with the hood. The
build deliberately compiles the central and observer roles out
(`CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED`), which is a large part of why NimBLE
fits at all.

There is also a report of writes destabilising a hood. @jens-42 wrote that
controlling a Skyline Edge Play over `f006f006` "frequently leads to crashes,
requiring me to flip the circuit breaker to restart the unit", with gesture
control (the laser dot) dead until then, while the two-byte writes to `f006f004`
were "absolutely reliable" for him. That is one report from one hood, not
reproduced here. Knowing now that the characteristic carries whole state rather
than commands makes a partial or stale array a plausible way to upset a hood,
but that is a hypothesis, not a finding.

[berbel-ha](https://github.com/dirkbloessl/berbel-ha) sends its light and fan
commands over `f006f006` (`WRITE_COMMANDS` in `berbel_ble/const.py`) and evidently
works for its users. Its byte offsets agree with the app; its names do not. What
it calls the top light is the effectlight and what it calls the bottom light is
the cooklight, and it has no notion of the third channel, the ambilight.

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
| [2] | 0x01 | Fan Power |
| [2] | 0x10 | Oberlicht (upper light) |
| [4] | 0x10 | Unterlicht (cooktop light) |
| [4] | 0x01 | Cover moving up (retracting) |
| [5] | 0x01 | Deckenlicht (ceiling connection light) |
| [5] | 0x90 | Nachlauf (afterrun timer active) |
| [6] | 0x01 | Cover moving down (deploying) |

Bytes [3], [7] and [8] are `0x00` in every frame observed so far, with one
exception described below. Nothing decodes them.

Fan Power is `byte[2] & 0x01` here as well, not the `0x09` this table used to
carry. A Skyline Frame switched to Power was read out over MQTT and sends `0x09`,
so both bits, which the narrower mask still matches:

```
16:07:07   00 00 09 D0 01 00 01 00 00    <- Power, descending from the park position
16:07:15   00 00 09 00 01 00 01 00 00
16:07:17   00 00 09 00 00 00 00 00 00    <- down, running
```

The same frames incidentally reconfirm two other things about this hood: the
`0xD0` in `byte[3]` during the automatic descent, and both movement flags set at
once while deploying.

### 13-byte frame (Skyline Edge Play)

Measured in [issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3).
The fan steps and the Unterlicht keep their positions; the Deckenlicht moves
from byte [5] to byte [9].

| Byte | Mask | Meaning | Confirmed |
|------|------|---------|-----------|
| [0] | 0x10 | Fan Stufe 1 | issue #3 and berbel-ha |
| [1] | 0x01 | Fan Stufe 2 | issue #3 and berbel-ha |
| [1] | 0x10 | Fan Stufe 3 | issue #3 and berbel-ha |
| [2] | 0x01 | Fan Power | **a BFB 7bT's own LEDs** |
| [2] | 0x08 | unknown; makes the Fan Power display blink | a BFB 7bT's own LEDs |
| [2] | 0x10 | Oberlicht (upper light) | berbel-ha |
| [4] | 0x10 | Unterlicht (cooktop light) | issue #3, berbel-ha, a BFB 7bT |
| [4] | 0x01 | Cover moving up (retracting) | issue #3 |
| [9] | 0x01 | Deckenlicht (ceiling connection light) | issue #3 |
| [5] | 0x90 | Nachlauf (afterrun timer active) | berbel-ha |
| [6] | 0x01 | Cover moving down (deploying) | issue #3 |

**Fan Power is `0x01`, not `0x09`.** It was the one field carried over from the
9-byte layout with nothing to back it, and
[berbel-ha](https://github.com/dirkbloessl/berbel-ha) (MIT), an independent Home
Assistant integration that reverse engineered the same hoods through a different
channel, still has it as `FAN_LEVEL_4 = 0x19  # Unknown (no level 4 available)`.
Measured against a real remote's own display, the two bits of the old `0x09`
mask turn out to be separate things:

| `byte[2]` | What the BFB 7bT shows |
|---|---|
| `0x01` | the P key lit, steady |
| `0x08` | nothing |
| `0x09` | the P key **blinking** |

So `0x01` is Fan Power and `0x08` is a modifier that changes how it is displayed
and means nothing on its own.

A real hood sets both. A Skyline Frame on Power sends `0x09`, which means the
blinking P key is what a remote normally shows for Power, and the steady one is
an artificial state no hood produces. That also makes the narrower mask safe:
the frames a hood actually sends still decode as Power, and only `0x08` without
`0x01`, which no hood has ever been seen to send, stops doing so.

What `0x08` stands for is still unknown. A time-limited intensive stage would fit
both a blinking indicator and a flag that only ever appears alongside Power, but
nothing here measured that.

Everything else in berbel-ha's `parser.py` agrees byte for byte and mask for mask
with the table above, which is where the "berbel-ha" confirmations come from.

### Reading the layout off a remote instead of a hood

The rows marked "a BFB 7bT's own LEDs" come from a method this project had not
used before: a remote lights its own keys from the status frames it receives, so
writing a frame and looking at the remote reads our decoding back to us. It needs
no hood at all, which matters because the 13-byte layout belongs to hoods almost
nobody doing this work owns.

Write one bit, look at the remote, write the next. With the [fake
hood](docs/fake-hood.md):

```bash
python3 fakehood.py ctl status 00000100000000000000000000
```

Two caveats. The remote is not the hood: this measures how a BFB 7bT *interprets*
a frame, which is the same decoding job this firmware does but not proof of what
a hood *sends*. And a webcam does not see these LEDs reliably in daylight, so the
reading is done by eye.

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
