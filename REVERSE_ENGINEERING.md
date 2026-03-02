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
| 0x0032 | f004f001-...-berbel | read, write-without-response | Hood -> Remote | **Hood status** (9-byte state packets) |
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

## Hood Status Protocol

The hood writes 9-byte status packets to `f004f001`. All values are bitmask-based.

| Byte | Mask | Meaning |
|------|------|---------|
| [0] | 0x10 | Fan Stufe 1 |
| [1] | 0x01 | Fan Stufe 2 |
| [1] | 0x10 | Fan Stufe 3 |
| [2] | 0x09 | Fan Power |
| [2] | 0x10 | Oberlicht (upper light) |
| [4] | 0x10 | Unterlicht (cooktop light) |
| [4] | 0x01 | Cover moving up (retracting) |
| [5] | 0x90 | Nachlauf (afterrun timer active) |
| [6] | 0x01 | Cover moving down (deploying) |

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
