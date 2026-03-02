#pragma once

// WiFi
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// MQTT
#define MQTT_HOST "192.168.1.x"
#define MQTT_PORT 1883
#define MQTT_USER "mqtt-user"
#define MQTT_PASS "mqtt-password"

// Hood features
// Set to false if your hood has no retractable cover (lift function).
// When false, Position, Hochfahren, Herunterfahren, and Cover State
// entities will not be created in Home Assistant.
#define HOOD_HAS_COVER true
