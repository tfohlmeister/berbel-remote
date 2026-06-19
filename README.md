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

The BFB 6bT remote works with Berbel hoods equipped with **berbel Connect 2.0**, manufactured from **November 2020** onwards. This emulator should work with any hood that supports the original remote.

**Island Hoods:** Skyline Frame, Skyline Edge, Skyline Curve, Skyline Sound, Skyline Light, Skyline Round, Ergoline, Glassline, Blockline, Smartline

**Wall-Mounted Hoods:** Glassline, Blockline, Smartline

**Headroom Hoods:** Ergoline, Glassline, Formline, Smartline

**Built-in Hoods (from April 2021):** Glassline, Firstline, Firstline Touch, Firstline Unseen

**Fan Modules (from April 2021):** Firstline

> **Note:** Hoods built before November 2020 are not compatible. If unsure, check the serial number with Berbel customer service.

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

2. **Edit `src/config.h`** with your WiFi and MQTT credentials. If your hood has no retractable cover (lift function), set `HOOD_HAS_COVER` to `false` to disable the Position, Hochfahren, Herunterfahren, and Cover State entities.

3. **Build and flash:**
   ```bash
   pio run -t upload        # USB (first flash)
   pio run -e ota -t upload # OTA (subsequent updates)
   ```

4. **Pair with the hood:**
   - Put the hood into pairing mode (on the Skyline Frame: hold the power and light buttons on the hood simultaneously for 5 seconds; other models may differ)
   - The ESP32 will connect automatically
   - The onboard LED stops blinking when connected

5. **Monitor:**
   ```bash
   pio device monitor
   ```

## Home Assistant Entities

All entities are created automatically via MQTT auto-discovery.

| Entity | Type | Description |
|--------|------|-------------|
| Oberlicht | Light | Upper/effect light toggle |
| Unterlicht | Light | Cooktop light toggle |
| Lufter | Select | Fan speed: Aus, Stufe 1-3, Power |
| Ausschalten | Button | Power off (starts afterrun timer) |
| Nachlauf | Switch | Toggle afterrun timer |
| Position | Select | Oben (retracted) / Unten (deployed) *(`HOOD_HAS_COVER` only)* |
| Hochfahren | Button | Move up unconditionally *(`HOOD_HAS_COVER` only)* |
| Herunterfahren | Button | Move down unconditionally *(`HOOD_HAS_COVER` only)* |
| BLE Verbindung | Binary Sensor | BLE connection status (diagnostic) |
| Cover State | Sensor | Cover position: up/moving up/moving down/down (diagnostic) *(`HOOD_HAS_COVER` only)* |
| Status Raw | Sensor | Raw 9-byte hex for debugging (diagnostic) |

## Button Codes

Complete mapping of all 13 buttons on the BFB 6bT remote control, with official function names from the Berbel manual.

| Code | Remote Label | Official Function (Berbel Manual) |
|------|-------------|-----------------------------------|
| 0x01 | Power | EIN/AUS |
| 0x02 | Fan 1 | Leistungsstufe 1 |
| 0x03 | Fan 2 | Leistungsstufe 2 |
| 0x04 | Fan 3 | Leistungsstufe 3 |
| 0x05 | Fan P | Leistungsstufe POWER |
| 0x06 | Cooktop Light | Kochfeld-Beleuchtung |
| 0x07 | Sync | Synchronisation |
| 0x08 | Recirculation | Umluftbetrieb / Kontrollanzeige Filter |
| 0x09 | Raise | Liftfunktion "Heben" |
| 0x0A | Effect Light | Effektbeleuchtung |
| 0x0B | Multi | Multifunktionstaste |
| 0x0C | Afterrun | Nachlauffunktion |
| 0x0D | Lower | Liftfunktion "Senken" |

Protocol: 2-byte notifications on characteristic `f004f002-...-berbel`. Press: `[code, 0x00]`, Release: `[0x00, 0x00]`.

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
