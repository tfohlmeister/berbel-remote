# Berbel Remote Emulator - BFB 6bT, 8bT and 7bT over BLE

[![tests](https://github.com/tfohlmeister/berbel-remote/actions/workflows/test.yml/badge.svg)](https://github.com/tfohlmeister/berbel-remote/actions/workflows/test.yml)

ESP32-based emulator for the Berbel BLE remote controls, with full Home Assistant
integration via MQTT. One build flag picks which remote to be:

| Profile | Remote | Keys | Reaches |
|---|---|---|---|
| `REMOTE_BFB_6BT` | **BFB 6bT** (Art. 1090045) | 13 | hoods with berbel Connect 2.0, from Nov 2020 |
| `REMOTE_BFB_8BT` | **BFB 8bT** (Art. 1090093) | 13 | the same hoods; the 6bT's replacement |
| `REMOTE_BFB_7BT` | **BFB 7bT** (Art. 1090084) | 19 | Skyline Edge Base and Play |

All three remotes were read out key by key over BLE, so the codes here are
measured rather than inferred. Developed and tested against a Berbel Skyline
Frame.

> **Disclaimer:** This is an **unofficial**, independent project created through reverse engineering.
> It is **not affiliated with, endorsed by, or connected to berbel Ablufttechnik GmbH**.
> "Berbel" is a trademark of berbel Ablufttechnik GmbH.
> Use at your own risk.

## Features

- **BLE Remote Emulation** - emulates the BFB 6bT, 8bT or 7bT, selected at build time
- **Home Assistant Integration** - MQTT auto-discovery creates entities automatically
- **Real-time Status Decoding** - reads the hood's status packets in both known frame layouts (lights, fan, position, afterrun)
- **OTA Updates** - wireless firmware updates via ArduinoOTA
- **NimBLE Stack** - ~100KB heap savings over Arduino BLE (Bluedroid), leaves room for WiFi + MQTT

## Which remote to emulate

Set `REMOTE_PROFILE` in `config.h` to the remote that came with your hood. It
defaults to `REMOTE_BFB_6BT`, so an existing `config.h` keeps working unchanged.

### BFB 6bT and BFB 8bT

The **BFB 6bT** (Art. 1090045) reaches hoods with **berbel Connect 2.0** and the
berbel ConInterface (Art. 1090043), built from **November 2020** onwards
(built-in models from April 2021).

berbel has since discontinued it and replaced it with the **BFB 8bT**
(Art. 1090093): same 13 keys, same functions, same hoods. Both were read out key
by key and send an identical set of codes, `0x01` to `0x0D`, with nothing left
over on either. The two profiles therefore send the same thing to the hood, and
which one you pick only changes the name this firmware gives itself: in the boot
log, and as the model of the device Home Assistant creates.

### BFB 7bT, for the Skyline Edge Base and Play

The **Skyline Edge Base (BIH SKEB)** and **Skyline Edge Play (BIH SKEP)** ship
with the **BFB 7bT** (Art. 1090084), which berbel lists as compatible with those
two hoods only. It has **19 keys against the other two's 13**: twelve they share,
no Synchronisation key, and seven of its own.

`REMOTE_BFB_7BT` switches those on. It points the Deckenlicht entity at the
Uplight code `0x12` instead of the multifunction key, and adds five buttons:

| Entity | Code | Function |
|---|---|---|
| Farbtemperatur Oberlicht | `0x15` | colour temperature of the Effektbeleuchtung |
| Farbtemperatur Unterlicht | `0x14` | colour temperature of the Kochfeld-Beleuchtung |
| Farbtemperatur Deckenlicht | `0x13` | colour temperature of the Uplight *(Play only)* |
| Motion-Lights | `0x16` | switch them on or off *(Play only)* |
| Motion-Lights weiterschalten | `0x17` | step to the next one *(Play only)* |

They are **buttons, not lights or selects**, and deliberately so. A colour
temperature key starts a sweep that a second press stops wherever it happens to
be, and the hood's status frame carries no field for the result, so there is no
state to track and an entity claiming one would be inventing it.

The 19th key is `0x0E`, which the manual itself lists as *ohne Funktion*. It
sends a code anyway, and gets no entity here because nothing is known to happen
when it is pressed. `berbel/hood/debug/send` reaches it on any build.

> **Untested against a hood.** The seven 7bT codes were read off a real remote
> and matched to its manual, which says what the *remote* sends. None of them has
> been driven at a Skyline Edge, because there is none here. If you have one, an
> issue saying what actually happens would be worth a lot.

Before this profile existed, Skyline Edge owners reached the ceiling light by
setting `HOOD_HAS_CEILING_LIGHT` to `true` and adding `#define BTN_LIGHT_CEILING
0x12` by hand. That still works and still wins over the profile default, so
nothing needs changing on an existing install.

Skyline Edge hoods also send a **13-byte status frame** instead of 9, which this
firmware decodes. Every field in it has now been measured, Fan Power last, which
turned out to sit on `byte[2] & 0x01` rather than the `0x09` this project long
assumed. See [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) for the table and
where each confirmation comes from.

All of the hood-side findings were measured on a Skyline Edge **Play**. The Base
was never tested here, though the independent
[berbel-ha](https://github.com/dirkbloessl/berbel-ha) integration decodes the
same status offsets on one.

> A note if you extend this firmware towards colour: characteristic `f006f006`
> is the colour and brightness channel, and there is a report of driving a
> Skyline Edge Play over it destabilising the hood. Not reproduced here, and this
> firmware never writes there, but worth handling with care. Details in
> [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).

### What a profile does not change

The identity this firmware presents to the hood stays the same for all three:
the spoofed Texas Instruments MAC, the Device Information strings and the GATT
device name. The real remotes differ in all three, and the profiles deliberately
do not follow them.

The reason is evidence. Skyline Edge owners already run this firmware with the
current identity and reach their hoods, so it is known to work where a 7bT would
be the original. The identities of the 8bT and 7bT were read off remotes that
were never paired to a hood here. Adopting them would trade a proven value for
an unproven one, and the way that failure would surface is a user whose hood
stops pairing.

## Hardware Requirements

- ESP32 development board (any variant with BLE + WiFi)
- Compatible Berbel kitchen hood (see list above)
- MQTT broker (e.g., Mosquitto)
- Home Assistant (optional, for smart home control)

## Quick Start

1. **Clone and configure:**
   ```bash
   git clone https://github.com/tfohlmeister/berbel-remote.git
   cd berbel-remote/BerbelRemote
   cp src/config.example.h src/config.h
   ```

2. **Edit `src/config.h`** with your WiFi and MQTT credentials, then match the feature flags to your hood:

   | Flag | Default | Set it when |
   |------|---------|-------------|
   | `REMOTE_PROFILE` | `REMOTE_BFB_6BT` | Pick the remote that came with your hood: `REMOTE_BFB_6BT`, `REMOTE_BFB_8BT` or `REMOTE_BFB_7BT`. See [Which remote to emulate](#which-remote-to-emulate). |
   | `HOOD_HAS_LIFT` | `true` | Your hood has no lift ("Liftfunktion Heben/Senken"): set `false` to drop the Position, Hochfahren, Herunterfahren and Cover State entities. Renamed from `HOOD_HAS_COVER`, which still works but warns at compile time. |
   | `HOOD_HAS_MULTI_BUTTON` | `false` | You assigned something to the multifunction button in the Berbel app: set `true` to add a Multifunktion button that presses it. |
   | `HOOD_HAS_CEILING_LIGHT` | `false` | Your hood has a ceiling connection with effect lighting (a third lamp) **and** the multifunction button is assigned to toggle it: set `true` to add the Deckenlicht entity. Any other assignment needs `HOOD_HAS_MULTI_BUTTON` instead, see [The multifunction button](#the-multifunction-button). |

3. **Build and flash:**
   ```bash
   pio run -t upload        # USB (first flash)
   pio run -e ota -t upload # OTA (subsequent updates)
   ```

   On an ESP32-S3, use the `esp32s3` and `ota-s3` environments instead. They also
   route the firmware log to the board's built-in USB-Serial-JTAG port, which the
   default ESP32 build leaves on UART0:
   ```bash
   pio run -e esp32s3 -t upload
   pio run -e ota-s3 -t upload
   ```

4. **Pair with the hood:**
   - Put the hood into pairing mode (on the Skyline Frame: hold the power and light buttons on the hood simultaneously for 5 seconds; other models may differ)
   - The ESP32 will connect automatically
   - The onboard LED stops blinking when connected

5. **Monitor:**
   ```bash
   pio device monitor
   ```

   Without a cable, turn on the **Remote Log** switch in Home Assistant and the
   firmware publishes the same output to MQTT:
   ```bash
   mosquitto_sub -h <broker> -u <user> -P <pass> -t 'berbel/hood/log'
   ```
   In Home Assistant the lines show up in the history of the **Log** entity, which
   ships disabled and has to be enabled once. Both steps, and how to read the
   result, are in [docs/remote-logging.md](docs/remote-logging.md).

## Home Assistant Entities

All entities are created automatically via MQTT auto-discovery.

| Entity | Type | Description |
|--------|------|-------------|
| Oberlicht | Light | Upper/effect light toggle |
| Unterlicht | Light | Cooktop light toggle |
| Deckenlicht | Light | Ceiling connection light toggle *(`HOOD_HAS_CEILING_LIGHT` only)* |
| Lufter | Select | Fan speed: Aus, Stufe 1-3, Power |
| Ausschalten | Button | Power off (starts afterrun timer) |
| Nachlauf | Switch | Toggle afterrun timer |
| Position | Select | Oben (retracted) / Unten (deployed) *(`HOOD_HAS_LIFT` only)* |
| Hochfahren | Button | Move up unconditionally *(`HOOD_HAS_LIFT` only)* |
| Herunterfahren | Button | Move down unconditionally *(`HOOD_HAS_LIFT` only)* |
| BLE Verbindung | Binary Sensor | BLE connection status (diagnostic) |
| Cover State | Sensor | Cover position: up/moving up/moving down/down (diagnostic) *(`HOOD_HAS_LIFT` only)* |
| Multifunktion | Button | Presses the multifunction key (only with `HOOD_HAS_MULTI_BUTTON`) |
| Farbtemperatur Oberlicht | Button | Start or stop the colour sweep of the Effektbeleuchtung *(`REMOTE_BFB_7BT` only)* |
| Farbtemperatur Unterlicht | Button | Same for the Kochfeld-Beleuchtung *(`REMOTE_BFB_7BT` only)* |
| Farbtemperatur Deckenlicht | Button | Same for the Uplight *(`REMOTE_BFB_7BT` only)* |
| Motion-Lights | Button | Switch the Motion-Lights on or off *(`REMOTE_BFB_7BT` only)* |
| Motion-Lights weiterschalten | Button | Step to the next Motion-Light *(`REMOTE_BFB_7BT` only)* |
| Status Raw | Sensor | Raw status frame as hex, 9 or 13 bytes (diagnostic) |
| Remote Log | Switch | Mirror the firmware log to MQTT (diagnostic) |
| Log | Sensor | Most recent firmware log line (diagnostic, disabled by default) |

### The multifunction button

Button `0x0B` is the one berbel leaves to you: what it does is assigned in the
**Berbel app**, and it is often a scene (set the hood to a height, the fan to a
level and the lights at once) rather than a simple toggle.

That matters for how it is exposed. `HOOD_HAS_MULTI_BUTTON` gives you a plain
**Multifunktion** button that presses it and nothing more, which is correct
whatever you assigned. `HOOD_HAS_CEILING_LIGHT` gives you a **Deckenlicht**
light entity instead, which only fits if you assigned a toggle to the button:
with an assignment such as "switch the lamp on", the entity has no way to switch
it off and gets stuck on
([issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3)).

The berbel manual also documents a long press (> 1 s) on that button as a
dimmer. This firmware always sends a short press, so dimming is not reachable
through the entities; `berbel/hood/debug/send` with `0B:1500` is the way to try
it.

## Button Codes

Complete mapping of all 13 buttons on the BFB 6bT remote control. The codes themselves are reverse engineered,
the function names come from the Berbel manual. Every code in the table has been measured; the Verified column
says where. Whether a hood performs a function at all depends on its equipment.

| Code | Function | Berbel Manual Term | Verified |
|------|----------|--------------------|----------|
| 0x01 | Power | EIN/AUS | on a hood, and on a BFB 8bT |
| 0x02 | Fan 1 | Leistungsstufe 1 | on a hood, and on a BFB 8bT |
| 0x03 | Fan 2 | Leistungsstufe 2 | on a hood, and on a BFB 8bT |
| 0x04 | Fan 3 | Leistungsstufe 3 | on a hood, and on a BFB 8bT |
| 0x05 | Fan P | Leistungsstufe POWER | on a hood, and on a BFB 8bT |
| 0x06 | Effect Light (Oberlicht) | Effektbeleuchtung | on a hood, and on a BFB 8bT |
| 0x07 | Sync | Synchronisation | on a BFB 8bT |
| 0x08 | Recirculation | Umluftbetrieb / Kontrollanzeige Filter-Füllung | on a BFB 8bT |
| 0x09 | Raise | Liftfunktion "Heben" | on a hood, and on a BFB 8bT |
| 0x0A | Cooktop Light (Unterlicht) | Kochfeld-Beleuchtung | on a hood, and on a BFB 8bT |
| 0x0B | Multifunction | Multifunktionstaste (assigned in the Berbel app, e.g. Deckenanschluss mit Effektbeleuchtung) | on a hood, and on a BFB 8bT |
| 0x0C | Afterrun | Nachlauffunktion | on a hood, and on a BFB 8bT |
| 0x0D | Lower | Liftfunktion "Senken" | on a hood, and on a BFB 8bT |
| 0x12 | Uplight / Ceiling Light | Uplight, Skyline Edge only; not a 6bT or 8bT button | on a Skyline Edge Play, and on a BFB 7bT |

`0x07` and `0x08` were the two rows this table used to mark "by elimination", because no hood here performs
either function. They are now read straight off a BFB 8bT and matched to the symbol table in that remote's
manual, which also confirms the guess had been right.

`0x01` to `0x0D` are the buttons of the BFB 6bT and, measured, of the BFB 8bT as well. The BFB 7bT adds
`0x0E` and `0x12` to `0x17` on top; the full table across all three remotes is in
[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).

A **long press sends no separate code**. The remote sends the ordinary press and delays the release, so the
hood measures the duration. Anything reachable by holding a key is therefore reachable from this firmware
too, through the hold time of `berbel/hood/debug/send`.

Protocol: 2-byte notifications on characteristic `f004f002-...-berbel`. Press: `[code, 0x00]`, Release: `[0x00, 0x00]`.

### Sending a raw button code

`berbel/hood/debug/send` presses any code, including ones this firmware does not
know. Use it to find what a function sits on when the table above does not fit
your hood:

```bash
mosquitto_pub -h <broker> -u <user> -P <pass> -t 'berbel/hood/debug/send' -m '0B'
mosquitto_pub -h <broker> -u <user> -P <pass> -t 'berbel/hood/debug/send' -m '0B:2000'
```

The payload is a hex code from `01` to `FF`, optionally followed by `:` and how
long to hold the button in milliseconds (1 to 5000, default 100). The long form
is for functions that may want a long press, such as a multifunction button.

Watch what a code does on the `Status Raw` sensor, or turn on remote logging and
read the `[HOOD] Status` lines directly (see
[docs/remote-logging.md](docs/remote-logging.md)).

A press the hood acts on produces a status frame within about half a second. No
frame means the hood did nothing, which is the useful signal when hunting for a
code, but it does not by itself prove the code is wrong: on a BFB 6bT hood,
`0A` (Kochfeld-Beleuchtung) switches the light on a short press and is ignored
entirely when held for two seconds. Both cases look the same from outside.

There is no state guard here: this sends the press whatever the hood is
currently doing, so a toggle really does toggle. Holding a button for seconds
stalls MQTT and OTA for that long, which is why the hold time is capped.

## Hood Status Bytes

The hood sends 9-byte status packets on characteristic `f004f001-...-berbel`. All values are bitmask-based.

| Byte | Mask | Meaning |
|------|------|---------|
| [0] | 0x10 | Fan Stufe 1 |
| [1] | 0x01 | Fan Stufe 2 |
| [1] | 0x10 | Fan Stufe 3 |
| [2] | 0x01 | Fan Power |
| [2] | 0x10 | Oberlicht (upper light) |
| [4] | 0x10 | Unterlicht (cooktop light) |
| [4] | 0x01 | Cover moving up (retracting) *(`HOOD_HAS_LIFT` only)* |
| [5] | 0x01 | Deckenlicht (ceiling connection light) *(`HOOD_HAS_CEILING_LIGHT` only)* |
| [5] | 0x90 | Nachlauf (afterrun timer active) |
| [6] | 0x01 | Cover moving down (deploying) *(`HOOD_HAS_LIFT` only)* |

A sync packet (all bytes `0x11`) is sent on connect and should be ignored.

## BLE Protocol Summary

### MAC Address OUI Filtering

The hood only accepts connections from devices with a Texas Instruments OUI. Four
remotes have been seen, carrying four different TI prefixes:
- `88:01:F9:xx:xx:xx`
- `30:AF:7E:xx:xx:xx`
- `FC:DE:C5:xx:xx:xx` (BFB 8bT)
- `0C:0A:DF:xx:xx:xx` (BFB 7bT)

Treat that as a sample rather than the whole list: the hood tests for the
manufacturer, not for these four values. Spoofing any of them works.

The ESP32 MAC is spoofed before BLE initialization:
```cpp
uint8_t ti_mac[6] = {0x88, 0x01, 0xF9, 0xAA, 0xBB, 0xCC};
esp_base_mac_addr_set(ti_mac);  // BEFORE NimBLEDevice::init()
```

### Pairing

- Legacy Pairing (no Secure Connections)
- Just Works (No Input, No Output)
- LTK only (no IRK, no CSRK)
- Hood is the Central (initiates pairing)

### GATT Service Order

Services must be created in this exact order (hood validates):
1. Device Information (0x180A)
2. Battery Service (0x180F)
3. HID Service (0x1812)
4. Berbel Custom Service (`f004f000-...-berbel`)

### Advertising

Raw `ADV_IND` with Flags + Service Data only. No device name, no HID UUID, no appearance. Service Data value must be `0x01` (active).

NimBLE keeps no copy of custom advertising data, it only flags that custom data is in use. A NimBLE host reset therefore clears the payload in the controller and the library restarts advertising empty, which the hood no longer matches. The firmware re-applies the raw payload every 30 seconds while no hood is connected.

## How It Was Reverse Engineered

1. Captured BLE traffic between the original remote and hood using an **nRF52840 Dongle** as a sniffer with **Wireshark/nRF Sniffer plugin**
2. Analyzed advertising data, GATT service structure, and SMP pairing exchange
3. Discovered MAC OUI filtering through trial and error (ESP32 with Espressif OUI was silently rejected)
4. Mapped all 13 button codes by pressing each button and recording notifications
5. Decoded 9-byte hood status packets by systematically toggling each function
6. Confirmed findings with a second remote (different TI OUI, identical protocol)
7. Built a **fake hood** and read a remote out directly, with no hood involved

Step 7 turns the arrangement around. The remote is the BLE peripheral and the hood
is the central, so a Linux box with a BLE adapter can play the hood: bond with a
remote, subscribe to its button characteristic and record every code it sends,
including codes no hood in reach would act on. The tool is
[`tools/fakehood/`](tools/fakehood/) and the method is written up in
[docs/fake-hood.md](docs/fake-hood.md).

See [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) for the full protocol documentation including GATT service tables, advertising data, and Wireshark analysis commands.

## Project Structure

```
berbel-remote/
├── BerbelRemote/              # ESP32 firmware (PlatformIO)
│   ├── src/
│   │   ├── main.cpp              # Firmware: BLE/WiFi/MQTT wiring
│   │   ├── berbel_protocol.h     # Pure protocol logic (unit-tested)
│   │   ├── remote_profile.h      # Which remote to emulate (6bT / 8bT / 7bT)
│   │   ├── config.example.h      # WiFi/MQTT config template
│   │   └── config.h              # Your credentials (gitignored)
│   ├── test/
│   │   └── test_protocol/        # Host-side unit tests (Unity)
│   └── platformio.ini            # Build configuration
├── tools/
│   └── fakehood/                 # Fake hood: read a remote out over BLE
├── docs/
│   ├── fake-hood.md              # Reading a remote out, and what it revealed
│   └── remote-logging.md         # Firmware log over MQTT
├── .github/workflows/test.yml     # CI: unit tests + firmware build
├── REVERSE_ENGINEERING.md         # Full protocol documentation
├── berbel_button_map.json        # Button code mapping (machine-readable)
├── LICENSE                       # MIT License
└── README.md
```

## Testing

The reverse-engineered protocol logic (status decoding, fan/cover state, JSON
parsing) lives in `src/berbel_protocol.h` as dependency-free functions, so it
can be unit-tested on the host without an ESP32:

```bash
cd BerbelRemote
pio test -e native
```

The fake hood's protocol half is separated the same way, in
`tools/fakehood/hood.py`, and imports no Bluetooth stack:

```bash
python3 -m unittest discover -s tools/fakehood -p "test_*.py"
```

CI runs both suites, a firmware compile check for every remote profile and
every hood feature flag combination, on each push (see the tests badge at the
top). The hardware I/O (BLE, WiFi, MQTT, OTA) is exercised on
the device, not in CI.

## Contributing

Contributions are welcome! If you have a different Berbel hood model and can capture BLE traffic, protocol comparisons would be especially valuable.

## License

This project is licensed under the [MIT License](LICENSE).

## Disclaimer

This is an unofficial community project based on private reverse engineering work. It is not affiliated with, endorsed by, or connected to **berbel Ablufttechnik GmbH** in any way.

All trademarks, including "Berbel", are the property of their respective owners and are used here solely to describe compatibility.

The authors take no responsibility for any damage to your hardware. Use at your own risk.
