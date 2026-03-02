/**
 * ============================================================================
 * Berbel BFB 6bT - Remote Control Emulator + Home Assistant Bridge
 * ============================================================================
 *
 * Emulates a Berbel kitchen hood remote control using an ESP32, with
 * WiFi/MQTT bridge to Home Assistant using MQTT Auto-Discovery.
 * Uses NimBLE stack for ~100KB heap savings over Arduino BLE (Bluedroid).
 *
 * BLE Protocol (reverse engineered):
 * - Button press:   [code, 0x00] as 2-byte notification on f004f002
 * - Button release: [0x00, 0x00] as 2-byte notification on f004f002
 * - Hood status:    9-byte writes from hood on f004f001
 * - Sync packet:    all 0x11 (ignored, sent on connect)
 *
 * Hood Status Bytes (9 bytes, bitmask-based):
 *   Byte[0] & 0x10  = Fan Stufe 1
 *   Byte[1] & 0x01  = Fan Stufe 2
 *   Byte[1] & 0x10  = Fan Stufe 3
 *   Byte[2] & 0x09  = Fan Power
 *   Byte[2] & 0x10  = Oberlicht (upper light)
 *   Byte[4] & 0x10  = Unterlicht (lower light)
 *   Byte[4] & 0x01  = Cover moving up (retracting)
 *   Byte[5] & 0x90  = Nachlauf (afterrun timer active)
 *   Byte[6] & 0x01  = Cover moving down (deploying)
 *
 * HA Entities (via MQTT Auto-Discovery):
 *   - Oberlicht       (light)          Toggle upper light
 *   - Unterlicht      (light)          Toggle lower light
 *   - Luefter         (select)         Fan speed: Aus, Stufe 1-3, Power
 *   - Ausschalten     (button)         Power off (starts Nachlauf)
 *   - Nachlauf        (switch)         Toggle afterrun timer
 *   - Position        (select)         Oben/Unten
 *   - BLE Verbindung  (binary_sensor)  BLE connection status
 *   - Cover State     (sensor)         Diagnostic: up/moving up/moving down/down
 *   - Status Raw      (sensor)         Raw 9-byte hex for debugging
 *
 * Critical requirements:
 * 1. MAC must use Texas Instruments OUI (88:01:F9 or 30:AF:7E)
 * 2. Advertising must include Service Data with UUID f000f000-...-berbel
 * 3. GATT services in correct order (DevInfo, Battery, HID, Berbel)
 * 4. Legacy Pairing, LTK only (no IRK)
 * 5. BLE must have radio priority (esp_coex_preference_set)
 *
 * ============================================================================
 */

#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_coexist.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

#include "config.h"

// ============================================================================
// Berbel Custom Service UUIDs
// ============================================================================
#define BERBEL_SERVICE_UUID   "f004f000-5745-4053-8043-62657262656c"
#define BERBEL_NOTIFY_UUID    "f004f002-5745-4053-8043-62657262656c"  // Button events (notify) + hood writes here too
#define BERBEL_WRITE_UUID     "f004f001-5745-4053-8043-62657262656c"  // Status from hood

// ============================================================================
// Button Codes
// ============================================================================
#define BTN_POWER       0x01
#define BTN_FAN_1       0x02
#define BTN_FAN_2       0x03
#define BTN_FAN_3       0x04
#define BTN_FAN_P       0x05
#define BTN_LIGHT_UP    0x06
#define BTN_PLAY        0x07
#define BTN_RELOAD      0x08
#define BTN_MOVE_UP     0x09
#define BTN_LIGHT_DOWN  0x0A
#define BTN_RECORD      0x0B
#define BTN_TIMER       0x0C
#define BTN_MOVE_DOWN   0x0D

// ============================================================================
// MQTT Topics
// ============================================================================
#define MQTT_BASE           "berbel/hood"
#define MQTT_AVAIL          MQTT_BASE "/available"
#define MQTT_STATE          MQTT_BASE "/state"
#define MQTT_CMD_LIGHT_UP   MQTT_BASE "/light_up/set"
#define MQTT_CMD_LIGHT_DOWN MQTT_BASE "/light_down/set"
#define MQTT_CMD_FAN_PRESET MQTT_BASE "/fan/preset/set"
#define MQTT_CMD_POWER      MQTT_BASE "/power/set"
#define MQTT_CMD_NACHLAUF   MQTT_BASE "/nachlauf/set"
#define MQTT_CMD_POSITION   MQTT_BASE "/position/set"
#define MQTT_CMD_MOVE_UP    MQTT_BASE "/move_up/set"
#define MQTT_CMD_MOVE_DOWN  MQTT_BASE "/move_down/set"
#define MQTT_CMD_DEBUG      MQTT_BASE "/debug/send"

// ============================================================================
// Hood State
// ============================================================================
struct HoodState {
  bool lightUp = false;
  bool lightDown = false;
  uint8_t fanSpeed = 0;  // 0=off, 1-4
  bool nachlauf = false;  // timer/afterrun active
  const char* position = "Oben";        // Oben, Unten, Fährt hoch, Fährt runter
  const char* coverState = "up";  // up, moving up, moving down, down
  bool bleConnected = false;
  uint8_t raw[9] = {0};
};

// ============================================================================
// Onboard LED (GPIO 2 on most ESP32 dev boards)
// ============================================================================
#define LED_PIN 2
#define LED_BLINK_MS 500  // blink interval when disconnected

// ============================================================================
// Globals
// ============================================================================
// BLE
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pNotifyChar = nullptr;
volatile bool deviceConnected = false;
bool oldDeviceConnected = false;

// WiFi/MQTT
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
HoodState hood;
unsigned long lastMqttReconnect = 0;
bool discoveryPublished = false;
bool wifiStarted = false;
bool otaReady = false;
bool hoodStateValid = false;  // true after first real status from hood (not sync)

// Status update from BLE callback (processed in loop)
volatile bool newStatusReceived = false;
uint8_t pendingStatus[9];

// Command queue (prevents commands from overlapping when scenes send multiple at once)
struct CmdEntry {
  uint8_t code;
  char name[16];
};
#define CMD_QUEUE_SIZE 16
#define CMD_DELAY_MS 300  // minimum ms between button presses
CmdEntry cmdQueue[CMD_QUEUE_SIZE];
int cmdQueueHead = 0;
int cmdQueueTail = 0;
unsigned long lastCmdSent = 0;

// ============================================================================
// Raw Advertising Data (must match real remote exactly!)
// ============================================================================
static uint8_t raw_adv_data[] = {
  0x02, 0x01, 0x05,                                      // Flags
  0x12, 0x21,                                             // Service Data 128-bit UUID
  0x6c, 0x65, 0x62, 0x72, 0x65, 0x62, 0x43, 0x80,
  0x53, 0x40, 0x45, 0x57, 0x00, 0xf0, 0x00, 0xf0,
  0x01                                                    // ACTIVE
};

// ============================================================================
// Forward Declarations
// ============================================================================
void sendButton(uint8_t code, const char* name);
void queueButton(uint8_t code, const char* name);
void processCmdQueue();
void publishState();
void publishDiscovery();
void startAdvertising();

// ============================================================================
// Start BLE Advertising with raw data
// ============================================================================
void startAdvertising() {
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->stop();

  NimBLEAdvertisementData advData;
  advData.addData(std::string(reinterpret_cast<const char*>(raw_adv_data), sizeof(raw_adv_data)));
  pAdvertising->setAdvertisementData(advData);

  // No scan response data
  NimBLEAdvertisementData scanData;
  pAdvertising->setScanResponseData(scanData);

  pAdvertising->setMinInterval(0x20);
  pAdvertising->setMaxInterval(0x40);

  pAdvertising->start();
  Serial.println("[BLE] Advertising started");
}

// ============================================================================
// BLE Callbacks
// ============================================================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("[BLE] Hood connected!");
  }

  void onDisconnect(NimBLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("[BLE] Hood disconnected");
    delay(100);
    startAdvertising();
  }
};

class WriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string value = pChar->getValue();
    Serial.printf("[HOOD] Status (%d bytes): ", value.length());
    for (size_t i = 0; i < value.length(); i++) {
      Serial.printf("%02X ", (uint8_t)value[i]);
    }
    Serial.println();

    if (value.length() == 9) {
      memcpy(pendingStatus, (const uint8_t*)value.data(), 9);
      newStatusReceived = true;
    }
  }
};

// ============================================================================
// Send Button Press/Release via BLE
// ============================================================================
void sendButton(uint8_t code, const char* name) {
  if (!deviceConnected || !pNotifyChar) {
    Serial.printf("[BTN] Cannot send %s - not connected\n", name);
    return;
  }

  Serial.printf("[BTN] Sending: %s (0x%02X)\n", name, code);

  uint8_t press[] = {code, 0x00};
  pNotifyChar->setValue(press, 2);
  pNotifyChar->notify();

  delay(100);

  uint8_t release[] = {0x00, 0x00};
  pNotifyChar->setValue(release, 2);
  pNotifyChar->notify();
}

// ============================================================================
// Command Queue (space out BLE commands for reliability)
// ============================================================================
void queueButton(uint8_t code, const char* name) {
  int next = (cmdQueueHead + 1) % CMD_QUEUE_SIZE;
  if (next == cmdQueueTail) {
    Serial.printf("[CMD] Queue full, dropping: %s\n", name);
    return;
  }
  cmdQueue[cmdQueueHead].code = code;
  strncpy(cmdQueue[cmdQueueHead].name, name, sizeof(cmdQueue[cmdQueueHead].name) - 1);
  cmdQueue[cmdQueueHead].name[sizeof(cmdQueue[cmdQueueHead].name) - 1] = '\0';
  cmdQueueHead = next;
  int pending = (cmdQueueHead - cmdQueueTail + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE;
  Serial.printf("[CMD] Queued: %s (0x%02X), pending: %d\n", name, code, pending);
}

void processCmdQueue() {
  if (cmdQueueHead == cmdQueueTail) return;  // empty
  if (!deviceConnected || !pNotifyChar) return;

  unsigned long now = millis();
  if (now - lastCmdSent < CMD_DELAY_MS) return;  // wait between commands

  CmdEntry& cmd = cmdQueue[cmdQueueTail];
  sendButton(cmd.code, cmd.name);
  cmdQueueTail = (cmdQueueTail + 1) % CMD_QUEUE_SIZE;
  lastCmdSent = millis();  // after sendButton (includes 100ms delay)
}

// ============================================================================
// MQTT State Publishing
// ============================================================================
static const char* fanPresetName(uint8_t speed) {
  switch (speed) {
    case 1: return "Stufe 1";
    case 2: return "Stufe 2";
    case 3: return "Stufe 3";
    case 4: return "Power";
    default: return "Aus";
  }
}

void publishState() {
  if (!mqtt.connected()) return;

  // Before first real status from hood, only publish BLE connection state.
  // This avoids overwriting the retained MQTT state with zeroed-out values
  // after a reboot. HA keeps showing the last known hood state.
  if (!hoodStateValid) {
    char json[64];
    snprintf(json, sizeof(json), "{\"ble\":\"%s\"}",
      hood.bleConnected ? "ON" : "OFF");
    mqtt.publish(MQTT_STATE, json, true);
    return;
  }

  char json[384];
  snprintf(json, sizeof(json),
    "{"
    "\"light_up\":\"%s\","
    "\"light_down\":\"%s\","
    "\"fan_preset\":\"%s\","
    "\"nachlauf\":\"%s\","
    "\"position\":\"%s\","
    "\"cover_state\":\"%s\","
    "\"ble\":\"%s\","
    "\"status_raw\":\"%02X %02X %02X %02X %02X %02X %02X %02X %02X\""
    "}",
    hood.lightUp ? "ON" : "OFF",
    hood.lightDown ? "ON" : "OFF",
    fanPresetName(hood.fanSpeed),
    hood.nachlauf ? "ON" : "OFF",
    hood.position,
    hood.coverState,
    hood.bleConnected ? "ON" : "OFF",
    hood.raw[0], hood.raw[1], hood.raw[2], hood.raw[3], hood.raw[4],
    hood.raw[5], hood.raw[6], hood.raw[7], hood.raw[8]);

  mqtt.publish(MQTT_STATE, json, true);
}

// ============================================================================
// HA MQTT Discovery
// ============================================================================
static const char DISCOVERY_DEVICE[] =
  ",\"avty_t\":\"" MQTT_AVAIL "\""
  ",\"dev\":{\"ids\":[\"berbel_hood\"]"
  ",\"name\":\"Berbel Hood\""
  ",\"mf\":\"Berbel\",\"mdl\":\"BFB 6bT\"}}";

void publishDiscoveryMsg(const char* topic, const char* fields) {
  char buf[768];
  snprintf(buf, sizeof(buf), "{%s%s", fields, DISCOVERY_DEVICE);
  mqtt.publish(topic, buf, true);
  delay(50);
}

void cleanupOldDiscovery() {
  // Remove old entity configs that no longer exist (empty payload = delete)
  const char* oldTopics[] = {
    "homeassistant/fan/berbel_hood/fan/config",
    "homeassistant/binary_sensor/berbel_hood/nachlauf/config",
    "homeassistant/cover/berbel_hood/cover/config",
    nullptr
  };
  for (int i = 0; oldTopics[i] != nullptr; i++) {
    mqtt.publish(oldTopics[i], "", true);
    delay(50);
  }
}

void publishDiscovery() {
  Serial.println("[MQTT] Publishing HA discovery...");
  cleanupOldDiscovery();

  // Light Up (Oberlicht)
  publishDiscoveryMsg(
    "homeassistant/light/berbel_hood/light_up/config",
    "\"name\":\"Oberlicht\","
    "\"uniq_id\":\"berbel_light_up\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"cmd_t\":\"" MQTT_CMD_LIGHT_UP "\","
    "\"stat_val_tpl\":\"{{ value_json.light_up }}\","
    "\"ic\":\"mdi:ceiling-light\""
  );

  // Light Down (Unterlicht)
  publishDiscoveryMsg(
    "homeassistant/light/berbel_hood/light_down/config",
    "\"name\":\"Unterlicht\","
    "\"uniq_id\":\"berbel_light_down\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"cmd_t\":\"" MQTT_CMD_LIGHT_DOWN "\","
    "\"stat_val_tpl\":\"{{ value_json.light_down }}\","
    "\"ic\":\"mdi:desk-lamp\""
  );

  // Fan (Luefter) - select entity for 5 speed levels
  publishDiscoveryMsg(
    "homeassistant/select/berbel_hood/fan/config",
    "\"name\":\"L\\u00fcfter\","
    "\"uniq_id\":\"berbel_fan\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"val_tpl\":\"{{ value_json.fan_preset }}\","
    "\"cmd_t\":\"" MQTT_CMD_FAN_PRESET "\","
    "\"ops\":[\"Aus\",\"Stufe 1\",\"Stufe 2\",\"Stufe 3\",\"Power\"],"
    "\"ic\":\"mdi:fan\""
  );

  // Position (select: Oben/Unten)
  publishDiscoveryMsg(
    "homeassistant/select/berbel_hood/position/config",
    "\"name\":\"Position\","
    "\"uniq_id\":\"berbel_position\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"val_tpl\":\"{{ value_json.position }}\","
    "\"cmd_t\":\"" MQTT_CMD_POSITION "\","
    "\"ops\":[\"Oben\",\"Unten\"],"
    "\"ic\":\"mdi:arrow-up-down\""
  );

  // BLE Connection Status (diagnostic)
  publishDiscoveryMsg(
    "homeassistant/binary_sensor/berbel_hood/ble/config",
    "\"name\":\"BLE Verbindung\","
    "\"uniq_id\":\"berbel_ble\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"val_tpl\":\"{{ value_json.ble }}\","
    "\"dev_cla\":\"connectivity\","
    "\"ent_cat\":\"diagnostic\""
  );

  // Power button (Ausschalten / Nachlauf starten)
  publishDiscoveryMsg(
    "homeassistant/button/berbel_hood/power/config",
    "\"name\":\"Ausschalten\","
    "\"uniq_id\":\"berbel_power\","
    "\"cmd_t\":\"" MQTT_CMD_POWER "\","
    "\"ic\":\"mdi:power\""
  );

  // Nachlauf (timer/afterrun toggle)
  publishDiscoveryMsg(
    "homeassistant/switch/berbel_hood/nachlauf/config",
    "\"name\":\"Nachlauf\","
    "\"uniq_id\":\"berbel_nachlauf\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"val_tpl\":\"{{ value_json.nachlauf }}\","
    "\"cmd_t\":\"" MQTT_CMD_NACHLAUF "\","
    "\"ic\":\"mdi:timer-sand\""
  );

  // Move Up button (unconditional)
  publishDiscoveryMsg(
    "homeassistant/button/berbel_hood/move_up/config",
    "\"name\":\"Hochfahren\","
    "\"uniq_id\":\"berbel_move_up\","
    "\"cmd_t\":\"" MQTT_CMD_MOVE_UP "\","
    "\"ic\":\"mdi:arrow-up\""
  );

  // Move Down button (unconditional)
  publishDiscoveryMsg(
    "homeassistant/button/berbel_hood/move_down/config",
    "\"name\":\"Herunterfahren\","
    "\"uniq_id\":\"berbel_move_down\","
    "\"cmd_t\":\"" MQTT_CMD_MOVE_DOWN "\","
    "\"ic\":\"mdi:arrow-down\""
  );

  // Cover State (diagnostic)
  publishDiscoveryMsg(
    "homeassistant/sensor/berbel_hood/cover_state/config",
    "\"name\":\"Cover State\","
    "\"uniq_id\":\"berbel_cover_state\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"val_tpl\":\"{{ value_json.cover_state }}\","
    "\"ent_cat\":\"diagnostic\","
    "\"ic\":\"mdi:arrow-up-down\""
  );

  // Raw Status (diagnostic, for reverse engineering)
  publishDiscoveryMsg(
    "homeassistant/sensor/berbel_hood/status_raw/config",
    "\"name\":\"Status Raw\","
    "\"uniq_id\":\"berbel_status_raw\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"val_tpl\":\"{{ value_json.status_raw }}\","
    "\"ent_cat\":\"diagnostic\","
    "\"ic\":\"mdi:bug\""
  );

  discoveryPublished = true;
  Serial.println("[MQTT] Discovery published!");
}

// ============================================================================
// Restore hood state from retained MQTT message (simple JSON parser)
// ============================================================================
static bool jsonGetValue(const char* json, const char* key, char* out, size_t outLen) {
  // Find "key":"value" in JSON string
  char search[32];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char* start = strstr(json, search);
  if (!start) return false;
  start += strlen(search);
  const char* end = strchr(start, '"');
  if (!end || (size_t)(end - start) >= outLen) return false;
  memcpy(out, start, end - start);
  out[end - start] = '\0';
  return true;
}

void restoreStateFromMqtt(const char* json) {
  char val[32];

  if (jsonGetValue(json, "light_up", val, sizeof(val)))
    hood.lightUp = (strcmp(val, "ON") == 0);
  if (jsonGetValue(json, "light_down", val, sizeof(val)))
    hood.lightDown = (strcmp(val, "ON") == 0);
  if (jsonGetValue(json, "nachlauf", val, sizeof(val)))
    hood.nachlauf = (strcmp(val, "ON") == 0);
  if (jsonGetValue(json, "fan_preset", val, sizeof(val))) {
    if (strcmp(val, "Stufe 1") == 0)      hood.fanSpeed = 1;
    else if (strcmp(val, "Stufe 2") == 0)  hood.fanSpeed = 2;
    else if (strcmp(val, "Stufe 3") == 0)  hood.fanSpeed = 3;
    else if (strcmp(val, "Power") == 0)    hood.fanSpeed = 4;
    else                                  hood.fanSpeed = 0;
  }
  if (jsonGetValue(json, "position", val, sizeof(val)))
    hood.position = (strcmp(val, "Unten") == 0) ? "Unten" : "Oben";
  if (jsonGetValue(json, "cover_state", val, sizeof(val))) {
    if (strcmp(val, "down") == 0)           hood.coverState = "down";
    else if (strcmp(val, "moving up") == 0) hood.coverState = "moving up";
    else if (strcmp(val, "moving down") == 0) hood.coverState = "moving down";
    else                                   hood.coverState = "up";
  }

  hoodStateValid = true;
  mqtt.unsubscribe(MQTT_STATE);
  Serial.printf("[MQTT] State restored: light_up=%d light_down=%d fan=%d nachlauf=%d pos=%s\n",
    hood.lightUp, hood.lightDown, hood.fanSpeed, hood.nachlauf, hood.position);
}

// ============================================================================
// MQTT Command Callback
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[384];
  unsigned int copyLen = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  memcpy(msg, payload, copyLen);
  msg[copyLen] = '\0';

  Serial.printf("[MQTT] %s = %s\n", topic, msg);

  String t(topic);

  // Restore state from retained message on startup
  if (t == MQTT_STATE && !hoodStateValid) {
    restoreStateFromMqtt(msg);
    return;
  }

  // Light Up (Oberlicht) - TOGGLE: check state before sending
  if (t == MQTT_CMD_LIGHT_UP) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.lightUp) {
      Serial.printf("[MQTT] Oberlicht already %s, skipping\n", msg);
      return;
    }
    queueButton(BTN_LIGHT_UP, "Light Up");
  }
  // Light Down (Unterlicht) - TOGGLE: check state before sending
  else if (t == MQTT_CMD_LIGHT_DOWN) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.lightDown) {
      Serial.printf("[MQTT] Unterlicht already %s, skipping\n", msg);
      return;
    }
    queueButton(BTN_LIGHT_DOWN, "Light Down");
  }
  // Power button (Ausschalten)
  else if (t == MQTT_CMD_POWER) {
    queueButton(BTN_POWER, "Power Off");
  }
  // Nachlauf - TOGGLE: check state before sending
  else if (t == MQTT_CMD_NACHLAUF) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.nachlauf) {
      Serial.printf("[MQTT] Nachlauf already %s, skipping\n", msg);
      return;
    }
    queueButton(BTN_TIMER, "Timer");
  }
  // Fan preset - check if already at target speed
  else if (t == MQTT_CMD_FAN_PRESET) {
    uint8_t targetSpeed = 0;
    uint8_t btnCode = BTN_POWER;
    const char* btnName = "Fan Off";

    if (strcmp(msg, "Stufe 1") == 0)      { targetSpeed = 1; btnCode = BTN_FAN_1; btnName = "Fan 1"; }
    else if (strcmp(msg, "Stufe 2") == 0)  { targetSpeed = 2; btnCode = BTN_FAN_2; btnName = "Fan 2"; }
    else if (strcmp(msg, "Stufe 3") == 0)  { targetSpeed = 3; btnCode = BTN_FAN_3; btnName = "Fan 3"; }
    else if (strcmp(msg, "Power") == 0)    { targetSpeed = 4; btnCode = BTN_FAN_P; btnName = "Fan Power"; }

    if (targetSpeed == hood.fanSpeed) {
      Serial.printf("[MQTT] Fan already at %s, skipping\n", msg);
      return;
    }
    queueButton(btnCode, btnName);
  }
  // Position (Oben/Unten)
  else if (t == MQTT_CMD_POSITION) {
    if (strcmp(msg, "Oben") == 0)        queueButton(BTN_MOVE_UP, "Move Up");
    else if (strcmp(msg, "Unten") == 0)  queueButton(BTN_MOVE_DOWN, "Move Down");
  }
  // Move Up button (unconditional, ignores tracked position)
  else if (t == MQTT_CMD_MOVE_UP) {
    queueButton(BTN_MOVE_UP, "Move Up");
  }
  // Move Down button (unconditional, ignores tracked position)
  else if (t == MQTT_CMD_MOVE_DOWN) {
    queueButton(BTN_MOVE_DOWN, "Move Down");
  }
  // HA restart - re-publish discovery
  else if (t == "homeassistant/status" && strcmp(msg, "online") == 0) {
    Serial.println("[MQTT] HA restarted, re-publishing discovery...");
    publishDiscovery();
    publishState();
  }
  // Debug: send raw button code (hex string like "0A")
  else if (t == MQTT_CMD_DEBUG) {
    uint8_t code = (uint8_t)strtol(msg, NULL, 16);
    if (code >= 0x01 && code <= 0x0D) {
      char name[16];
      snprintf(name, sizeof(name), "Debug 0x%02X", code);
      queueButton(code, name);
    }
  }
}

// ============================================================================
// WiFi Setup
// ============================================================================
void setupWiFi() {
  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("berbel-remote");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  // OTA callbacks (set once, begin() called when WiFi is ready)
  ArduinoOTA.setHostname("berbel-remote");
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Update starting, switching to WiFi priority...");
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete, restoring BLE priority...");
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] %u%%\r", progress * 100 / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error %u\n", error);
  });

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    ArduinoOTA.begin();
    otaReady = true;
    Serial.println("[OTA] Ready");
  } else {
    Serial.println("\n[WiFi] Connection failed, will retry in loop");
  }
}

// ============================================================================
// MQTT Reconnect
// ============================================================================
void mqttReconnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttReconnect < 5000) return;
  lastMqttReconnect = now;

  Serial.printf("[MQTT] Connecting to %s:%d...\n", MQTT_HOST, MQTT_PORT);

  if (mqtt.connect("berbel-remote", MQTT_USER, MQTT_PASS,
                    MQTT_AVAIL, 0, true, "offline")) {
    Serial.println("[MQTT] Connected!");
    mqtt.publish(MQTT_AVAIL, "online", true);

    mqtt.subscribe(MQTT_CMD_LIGHT_UP);
    mqtt.subscribe(MQTT_CMD_LIGHT_DOWN);
    mqtt.subscribe(MQTT_CMD_POWER);
    mqtt.subscribe(MQTT_CMD_NACHLAUF);
    mqtt.subscribe(MQTT_CMD_FAN_PRESET);
    mqtt.subscribe(MQTT_CMD_POSITION);
    mqtt.subscribe(MQTT_CMD_MOVE_UP);
    mqtt.subscribe(MQTT_CMD_MOVE_DOWN);
    mqtt.subscribe(MQTT_CMD_DEBUG);
    mqtt.subscribe("homeassistant/status");

    // Subscribe to own state topic to restore state from retained message
    if (!hoodStateValid) {
      mqtt.subscribe(MQTT_STATE);
      Serial.println("[MQTT] Subscribed to state topic for restore...");
    }

    publishDiscovery();
    publishState();
  } else {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
  }
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // LED setup - starts blinking (disconnected state)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.println("\n============================================");
  Serial.println("  BERBEL REMOTE - HA Bridge (NimBLE)");
  Serial.println("============================================\n");

  // ----- BLE Setup (must come first for MAC spoofing) -----

  Serial.println("[MAC] Setting Texas Instruments OUI...");
  uint8_t ti_mac[6] = {0x88, 0x01, 0xF9, 0xAA, 0xBB, 0xCC};
  esp_base_mac_addr_set(ti_mac);

  Serial.println("[BLE] Initializing NimBLE...");
  NimBLEDevice::init("");

  Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());

  // Security: Legacy Pairing, LTK only (no IRK)
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  // --- 1. Device Information Service (0x180A) ---
  NimBLEService* pDevInfoService = pServer->createService(NimBLEUUID((uint16_t)0x180A));
  NimBLECharacteristic* pManufacturer = pDevInfoService->createCharacteristic(
      NimBLEUUID((uint16_t)0x2A29), NIMBLE_PROPERTY::READ);
  pManufacturer->setValue("Texas Instruments");
  NimBLECharacteristic* pPnpId = pDevInfoService->createCharacteristic(
      NimBLEUUID((uint16_t)0x2A50), NIMBLE_PROPERTY::READ);
  uint8_t pnpId[] = {0x01, 0x0D, 0x00, 0x00, 0x00, 0x10, 0x00};
  pPnpId->setValue(pnpId, 7);
  pDevInfoService->start();

  // --- 2. Battery Service (0x180F) ---
  NimBLEService* pBatteryService = pServer->createService(NimBLEUUID((uint16_t)0x180F));
  NimBLECharacteristic* pBatteryLevel = pBatteryService->createCharacteristic(
      NimBLEUUID((uint16_t)0x2A19),
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t batteryLevel = 90;
  pBatteryLevel->setValue(&batteryLevel, 1);
  pBatteryService->start();

  // --- 3. HID Service (0x1812) ---
  NimBLEService* pHidService = pServer->createService(NimBLEUUID((uint16_t)0x1812));

  NimBLECharacteristic* pHidInfo = pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4A), NIMBLE_PROPERTY::READ);
  uint8_t hidInfo[] = {0x11, 0x01, 0x00, 0x01};
  pHidInfo->setValue(hidInfo, 4);

  pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4C), NIMBLE_PROPERTY::WRITE_NR);

  NimBLECharacteristic* pProtocol = pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4E),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR);
  uint8_t protocolMode = 0x01;
  pProtocol->setValue(&protocolMode, 1);

  NimBLECharacteristic* pReportMap = pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4B), NIMBLE_PROPERTY::READ);
  const uint8_t reportMap[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0xE0, 0x15, 0xE8, 0x25, 0x18, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06, 0xC0
  };
  pReportMap->setValue(reportMap, sizeof(reportMap));

  NimBLECharacteristic* pReport = pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4D),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  // Report Reference descriptor
  NimBLEDescriptor* pReportRef = pReport->createDescriptor(
    NimBLEUUID((uint16_t)0x2908), NIMBLE_PROPERTY::READ);
  uint8_t reportRef[] = {0x01, 0x01};
  pReportRef->setValue(reportRef, 2);

  pHidService->start();

  // --- 4. Berbel Custom Service ---
  NimBLEService* pBerbelService = pServer->createService(BERBEL_SERVICE_UUID);

  // f004f002: Button notifications TO hood + hood also writes here (discovered from capture)
  pNotifyChar = pBerbelService->createCharacteristic(
    BERBEL_NOTIFY_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE_NR);
  uint8_t initVal[] = {0x00, 0x00};
  pNotifyChar->setValue(initVal, 2);

  // f004f001: Status writes FROM hood
  NimBLECharacteristic* pWriteChar = pBerbelService->createCharacteristic(
    BERBEL_WRITE_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR);
  pWriteChar->setCallbacks(new WriteCallbacks());
  pWriteChar->setValue(initVal, 2);

  pBerbelService->start();

  // Start advertising with raw data
  startAdvertising();
  Serial.println("[BLE] Services started, advertising...");

  // Prioritize BLE over WiFi on the shared radio
  esp_coex_preference_set(ESP_COEX_PREFER_BT);

  // WiFi + MQTT - start immediately (NimBLE has enough heap headroom)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);

  Serial.printf("[SYS] Free heap before WiFi: %u bytes\n", esp_get_free_heap_size());
  setupWiFi();
  Serial.printf("[SYS] Free heap after WiFi: %u bytes\n", esp_get_free_heap_size());
  wifiStarted = true;

  Serial.println("\n============================================");
  Serial.println("  Ready! Waiting for hood...");
  Serial.println("============================================\n");
}

// ============================================================================
// Main Loop
// ============================================================================
void loop() {
  // --- Heap monitoring (every 30s) ---
  static unsigned long lastHeapLog = 0;
  unsigned long now = millis();
  if (now - lastHeapLog > 30000) {
    lastHeapLog = now;
    Serial.printf("[SYS] Free heap: %u bytes, BLE: %s, WiFi: %s\n",
      esp_get_free_heap_size(),
      deviceConnected ? "connected" : "waiting",
      wifiStarted ? (WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") : "off");
  }

  // --- OTA ---
  if (wifiStarted && WiFi.status() == WL_CONNECTED) {
    if (!otaReady) {
      ArduinoOTA.begin();
      otaReady = true;
      Serial.printf("[OTA] Ready (late init), IP: %s\n", WiFi.localIP().toString().c_str());
    }
    ArduinoOTA.handle();
  }

  // --- WiFi reconnect ---
  if (wifiStarted && WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiRetry = 0;
    if (now - lastWifiRetry > 30000) {
      lastWifiRetry = now;
      Serial.println("[WiFi] Reconnecting...");
      WiFi.reconnect();
    }
  }

  // --- MQTT ---
  if (wifiStarted) {
    if (!mqtt.connected()) {
      mqttReconnect();
    } else {
      mqtt.loop();
    }
  }

  // --- LED: off when connected, blink when disconnected ---
  if (deviceConnected) {
    digitalWrite(LED_PIN, LOW);
  } else {
    static unsigned long lastLedToggle = 0;
    if (now - lastLedToggle >= LED_BLINK_MS) {
      lastLedToggle = now;
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  }

  // --- BLE connection state change ---
  if (deviceConnected != oldDeviceConnected) {
    hood.bleConnected = deviceConnected;
    if (!deviceConnected) {
      // Keep last known hood state across disconnects - the hood will
      // send a fresh status update after reconnecting (after sync packet).
      // Only flush pending commands since they can't be delivered anyway.
      cmdQueueHead = cmdQueueTail = 0;
    }
    publishState();
    oldDeviceConnected = deviceConnected;
  }

  // --- Process command queue (spaced out button presses) ---
  processCmdQueue();

  // --- Process status update from hood (set in BLE callback) ---
  if (newStatusReceived) {
    newStatusReceived = false;

    // Skip the sync packet (all 0x11) entirely
    bool isSync = true;
    for (int i = 0; i < 9; i++) {
      if (pendingStatus[i] != 0x11) { isSync = false; break; }
    }
    if (isSync) {
      Serial.println("[HOOD] Sync packet ignored");
    } else {
      hoodStateValid = true;
      memcpy(hood.raw, pendingStatus, 9);

      // Lights (bitmask - bits don't overlap with fan)
      hood.lightUp   = (hood.raw[2] & 0x10);  // Oberlicht: bit 4
      hood.lightDown = (hood.raw[4] & 0x10);  // Unterlicht: bit 4

      // Fan speed (bitmask - only one active at a time)
      if (hood.raw[2] & 0x09)               hood.fanSpeed = 4;  // Power: 0000 1001
      else if (hood.raw[1] & 0x10)          hood.fanSpeed = 3;  // Stufe 3: 0001 0000
      else if (hood.raw[1] & 0x01)          hood.fanSpeed = 2;  // Stufe 2: 0000 0001
      else if (hood.raw[0] & 0x10)          hood.fanSpeed = 1;  // Stufe 1: 0001 0000
      else                                  hood.fanSpeed = 0;  // Aus

      // Nachlauf (parallel to fan speed)
      hood.nachlauf = (hood.raw[5] & 0x90);

      // Cover state (byte[4] bit 0 = moving up, byte[6] bit 0 = moving down)
      if (hood.raw[4] & 0x01) {
        hood.coverState = "moving up";
        hood.position = "Oben";
      } else if (hood.raw[6] & 0x01) {
        hood.coverState = "moving down";
        hood.position = "Unten";
      } else if (strcmp(hood.coverState, "moving up") == 0) {
        hood.coverState = "up";
      } else if (strcmp(hood.coverState, "moving down") == 0) {
        hood.coverState = "down";
      }

      publishState();
    }
  }

  delay(10);
}
