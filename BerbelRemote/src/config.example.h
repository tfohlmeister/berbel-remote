#pragma once

// WiFi
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// MQTT
#define MQTT_HOST "192.168.1.x"
#define MQTT_PORT 1883
#define MQTT_USER "mqtt-user"
#define MQTT_PASS "mqtt-password"

// Status LED. Defaults to GPIO 2, the onboard LED of most classic ESP32 dev
// boards. The ESP32-S3-DevKitC-1 has only an addressable RGB LED, which this
// firmware cannot drive, so there the indicator stays dark unless you wire a
// plain LED to a free pin and name it here.
// #define LED_PIN 2

// Mirror the firmware log to the MQTT topic berbel/hood/log. Off here means the
// log can still be switched on at runtime through the "Remote Log" switch in
// Home Assistant; that choice is retained and survives a reboot.
#define REMOTE_LOG_DEFAULT false

// Hood features
// Set to false if your hood has no lift ("Liftfunktion Heben/Senken").
// When false, Position, Hochfahren, Herunterfahren, and Cover State
// entities will not be created in Home Assistant.
// This flag used to be called HOOD_HAS_COVER. The old name still works and
// prints a compiler warning; rename it here to silence that.
#define HOOD_HAS_LIFT true

// The multifunction button (code 0x0B) does whatever you assigned to it in the
// Berbel app: a light, or a scene that sets height, fan level and lighting at
// once. Set this to true to get a "Multifunktion" button in Home Assistant that
// simply presses it.
#define HOOD_HAS_MULTI_BUTTON false

// Set to true if your hood has a third lamp on a ceiling connection
// ("Deckenanschluss mit Effektbeleuchtung", called Uplight in newer berbel
// manuals), to get a Deckenlicht light entity for it.
//
// A light entity only fits a button that toggles the lamp, since it tracks the
// state and presses the button when that state should change. Which button that
// is depends on the hood, see BTN_LIGHT_CEILING below. On a hood where the lamp
// hangs off the multifunction key, that key must be assigned a toggle in the
// Berbel app; with any other assignment (switching the lamp on without ever
// switching it off, or a scene) the entity gets stuck and you want
// HOOD_HAS_MULTI_BUTTON instead.
#define HOOD_HAS_CEILING_LIGHT false

// Which button code the Deckenlicht entity presses. The default is the
// multifunction key (0x0B), which is how a BFB 6bT hood drives that lamp.
// A Skyline Edge Play has a dedicated code for it instead, verified on real
// hardware in issue #3, and presumably the Skyline Edge Base does too:
//   #define BTN_LIGHT_CEILING 0x12
// If your hood does neither, berbel/hood/debug/send can find the code, see
// the README.
