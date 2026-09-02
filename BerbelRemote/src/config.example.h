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
// Set to false if your hood has no retractable cover (lift function).
// When false, Position, Hochfahren, Herunterfahren, and Cover State
// entities will not be created in Home Assistant.
#define HOOD_HAS_COVER true

// Set to true if your hood has a ceiling connection with effect lighting
// ("Deckenanschluss mit Effektbeleuchtung"), the optional third lamp driven
// by the multifunction button. When false, the Deckenlicht entity will not
// be created in Home Assistant.
#define HOOD_HAS_CEILING_LIGHT false
