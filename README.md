# Berbel BFB 6bT - BLE Remote Control Emulator

[![tests](https://github.com/tfohlmeister/berbel-remote/actions/workflows/test.yml/badge.svg)](https://github.com/tfohlmeister/berbel-remote/actions/workflows/test.yml)

ESP32-based emulator for the **Berbel BFB 6bT** remote control (Art. 1090045), with full Home Assistant integration via MQTT. Tested with a Berbel Skyline Frame hood, but the BFB 6bT remote is compatible with other Berbel hoods as well.

> **Disclaimer:** This is an **unofficial**, independent project created through reverse engineering.
> It is **not affiliated with, endorsed by, or connected to berbel Ablufttechnik GmbH**.
> "Berbel" is a trademark of berbel Ablufttechnik GmbH.
> Use at your own risk.

## Features

- **BLE Remote Emulation** - Fully emulates the original Berbel BFB 6bT remote control
- **Home Assistant Integration** - MQTT auto-discovery creates entities automatically
- **Real-time Status Decoding** - Reads 9-byte status packets from the hood (lights, fan, position, afterrun)
- **OTA Updates** - Wireless firmware updates via ArduinoOTA
- **NimBLE Stack** - ~100KB heap savings over Arduino BLE (Bluedroid), leaves room for WiFi + MQTT

## Compatible Hoods

This firmware emulates the **BFB 6bT** remote (Art. 1090045), so it reaches the
hoods that remote reaches: those with **berbel Connect 2.0** and the berbel
ConInterface (Art. 1090043), built from **November 2020** onwards (built-in
models from April 2021). Developed and tested against a Berbel Skyline Frame.

berbel has since discontinued the 6bT and replaced it with the **BFB 8bT**
(Art. 1090093), which carries the same 13 buttons and the same functions for the
same range of hoods. Untested here, but nothing suggests it needs a different
emulation.

### Where this firmware falls short

The **Skyline Edge Base (BIH SKEB)** and **Skyline Edge Play (BIH SKEP)** ship
with the **BFB 7bT** (Art. 1090084) instead, and berbel lists that remote as
compatible with those two hoods only. Those hoods do pair with this firmware and
do respond to the basic functions, but not to everything:

* The 7bT has **19 buttons against the 6bT's 13**, among them Uplight, Motion
  Lights, colour temperature for each light and a favourite scene. This firmware
  has no button code for any of them, because the remote it emulates has no such
  key.
* Conversely the 6bT's **Multifunktionstaste** and **Synchronisation** do not
  exist on the 7bT. The key in that position is listed in the berbel manual as
  "ohne Funktion", which is why sending `0x0B` to a Skyline Edge does nothing
  ([issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3)).
* Their status frame is longer and lays some flags out differently, see
  [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).

These hoods also expose a GATT server of their own, which is a different
protocol from the one here and is handled by other projects. If you want to map
the missing functions for a Skyline Edge, `berbel/hood/debug/send` is the tool
for it and the findings are welcome in an issue.

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
   | `HOOD_HAS_COVER` | `true` | Your hood has no retractable cover (lift function): set `false` to drop the Position, Hochfahren, Herunterfahren and Cover State entities. |
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
| Position | Select | Oben (retracted) / Unten (deployed) *(`HOOD_HAS_COVER` only)* |
| Hochfahren | Button | Move up unconditionally *(`HOOD_HAS_COVER` only)* |
| Herunterfahren | Button | Move down unconditionally *(`HOOD_HAS_COVER` only)* |
| BLE Verbindung | Binary Sensor | BLE connection status (diagnostic) |
| Cover State | Sensor | Cover position: up/moving up/moving down/down (diagnostic) *(`HOOD_HAS_COVER` only)* |
| Multifunktion | Button | Presses the multifunction key (only with `HOOD_HAS_MULTI_BUTTON`) |
| Status Raw | Sensor | Raw 9-byte hex for debugging (diagnostic) |
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
the function names come from the Berbel manual (document 6005229_0). Codes marked as verified were confirmed by
driving them on a real hood; the rest are assigned by elimination and may still be swapped. Whether a hood
performs a function at all depends on its equipment.

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

The table covers the BFB 6bT. Other remotes may use codes beyond `0x0D`, or put a
function somewhere else entirely: on a BFB 7bT hood, `0x0B` does nothing at all
while the original remote still switches its ceiling light
([issue #3](https://github.com/tfohlmeister/berbel-remote/issues/3)).

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
| [2] | 0x09 | Fan Power |
| [2] | 0x10 | Oberlicht (upper light) |
| [4] | 0x10 | Unterlicht (cooktop light) |
| [4] | 0x01 | Cover moving up (retracting) *(`HOOD_HAS_COVER` only)* |
| [5] | 0x01 | Deckenlicht (ceiling connection light) *(`HOOD_HAS_CEILING_LIGHT` only)* |
| [5] | 0x90 | Nachlauf (afterrun timer active) |
| [6] | 0x01 | Cover moving down (deploying) *(`HOOD_HAS_COVER` only)* |

A sync packet (all bytes `0x11`) is sent on connect and should be ignored.

## BLE Protocol Summary

### MAC Address OUI Filtering

The hood only accepts connections from devices with a Texas Instruments OUI:
- `88:01:F9:xx:xx:xx`
- `30:AF:7E:xx:xx:xx`

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

See [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) for the full protocol documentation including GATT service tables, advertising data, and Wireshark analysis commands.

## Project Structure

```
berbel-remote/
├── BerbelRemote/              # ESP32 firmware (PlatformIO)
│   ├── src/
│   │   ├── main.cpp              # Firmware: BLE/WiFi/MQTT wiring
│   │   ├── berbel_protocol.h     # Pure protocol logic (unit-tested)
│   │   ├── config.example.h      # WiFi/MQTT config template
│   │   └── config.h              # Your credentials (gitignored)
│   ├── test/
│   │   └── test_protocol/        # Host-side unit tests (Unity)
│   └── platformio.ini            # Build configuration
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

CI runs these tests and a full firmware compile check on every push (see the
tests badge at the top). The hardware I/O (BLE, WiFi, MQTT, OTA) is exercised on
the device, not in CI.

## Contributing

Contributions are welcome! If you have a different Berbel hood model and can capture BLE traffic, protocol comparisons would be especially valuable.

## License

This project is licensed under the [MIT License](LICENSE).

## Disclaimer

This is an unofficial community project based on private reverse engineering work. It is not affiliated with, endorsed by, or connected to **berbel Ablufttechnik GmbH** in any way.

All trademarks, including "Berbel", are the property of their respective owners and are used here solely to describe compatibility.

The authors take no responsibility for any damage to your hardware. Use at your own risk.
