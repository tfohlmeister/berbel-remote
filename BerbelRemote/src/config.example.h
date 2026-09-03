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

// Set to true if your hood has a ceiling connection with effect lighting
// ("Deckenanschluss mit Effektbeleuchtung", called Uplight in newer berbel
// manuals) AND the multifunction button is assigned to toggle it. Only then is
// a light entity the right fit: it tracks the lamp's state and sends the button
// when that state should change. If the app assigns anything else to the button
// (turning the lamp on without turning it off again, or a scene), use
// HOOD_HAS_MULTI_BUTTON instead, or the entity gets stuck on.
#define HOOD_HAS_CEILING_LIGHT false
