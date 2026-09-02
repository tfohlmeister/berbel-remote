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
 *   Byte[5] & 0x01  = Deckenlicht (ceiling connection light)
 *   Byte[5] & 0x90  = Nachlauf (afterrun timer active)
 *   Byte[6] & 0x01  = Cover moving down (deploying)
 *
 * HA Entities (via MQTT Auto-Discovery):
 *   - Oberlicht       (light)          Toggle upper light
 *   - Unterlicht      (light)          Toggle lower light
 *   - Deckenlicht     (light)          Toggle ceiling connection light (only with HOOD_HAS_CEILING_LIGHT)
 *   - Luefter         (select)         Fan speed: Aus, Stufe 1-3, Power
 *   - Ausschalten     (button)         Power off (starts Nachlauf)
 *   - Nachlauf        (switch)         Toggle afterrun timer
 *   - Position        (select)         Oben/Unten (only with HOOD_HAS_COVER)
 *   - Hochfahren      (button)         Move up unconditionally (only with HOOD_HAS_COVER)
 *   - Herunterfahren  (button)         Move down unconditionally (only with HOOD_HAS_COVER)
 *   - BLE Verbindung  (binary_sensor)  BLE connection status
 *   - Cover State     (sensor)         Diagnostic: up/moving up/moving down/down (only with HOOD_HAS_COVER)
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
#include "berbel_protocol.h"

// Feature flags added after the initial release. A config.h from an older
// checkout does not define them, so default them to off here rather than
// relying on the preprocessor treating the unknown name as 0.
#ifndef HOOD_HAS_CEILING_LIGHT
#define HOOD_HAS_CEILING_LIGHT false
#endif

// Whether remote logging starts enabled. The retained MQTT state overrides this
// on boot, so a device that was switched on stays on across a reboot.
#ifndef REMOTE_LOG_DEFAULT
#define REMOTE_LOG_DEFAULT false
#endif

// ============================================================================
// Berbel Custom Service UUIDs
// ============================================================================
#define BERBEL_SERVICE_UUID   "f004f000-5745-4053-8043-62657262656c"
#define BERBEL_NOTIFY_UUID    "f004f002-5745-4053-8043-62657262656c"  // Button events (notify) + hood writes here too
#define BERBEL_WRITE_UUID     "f004f001-5745-4053-8043-62657262656c"  // Status from hood

// ============================================================================
// Button Codes (named after the function in the Berbel BFB 6bT manual)
// ============================================================================
#define BTN_POWER       0x01
#define BTN_FAN_1       0x02
#define BTN_FAN_2       0x03
#define BTN_FAN_3       0x04
#define BTN_FAN_P       0x05
#define BTN_LIGHT_UP    0x06
#define BTN_SYNC        0x07
#define BTN_RECIRC      0x08
#define BTN_MOVE_UP     0x09
#define BTN_LIGHT_DOWN  0x0A
#define BTN_MULTI       0x0B  // multifunction, drives the ceiling connection light
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
#if HOOD_HAS_CEILING_LIGHT
#define MQTT_CMD_LIGHT_CEILING MQTT_BASE "/light_ceiling/set"
#endif
#define MQTT_CMD_FAN_PRESET MQTT_BASE "/fan/preset/set"
#define MQTT_CMD_POWER      MQTT_BASE "/power/set"
#define MQTT_CMD_NACHLAUF   MQTT_BASE "/nachlauf/set"
#if HOOD_HAS_COVER
#define MQTT_CMD_POSITION   MQTT_BASE "/position/set"
#define MQTT_CMD_MOVE_UP    MQTT_BASE "/move_up/set"
#define MQTT_CMD_MOVE_DOWN  MQTT_BASE "/move_down/set"
#endif
#define MQTT_CMD_DEBUG      MQTT_BASE "/debug/send"
#define MQTT_LOG            MQTT_BASE "/log"
#define MQTT_LOG_STATE      MQTT_BASE "/log/state"
#define MQTT_CMD_LOG        MQTT_BASE "/log/set"

// ============================================================================
// Hood State
// ============================================================================
struct HoodState {
  bool lightUp = false;
  bool lightDown = false;
#if HOOD_HAS_CEILING_LIGHT
  bool lightCeiling = false;
#endif
  uint8_t fanSpeed = 0;  // 0=off, 1-4
  bool nachlauf = false;  // timer/afterrun active
#if HOOD_HAS_COVER
  const char* position = "Oben";        // Oben, Unten, Fährt hoch, Fährt runter
  const char* coverState = "up";  // up, moving up, moving down, down
#endif
  bool bleConnected = false;
  uint8_t raw[9] = {0};
};

// ============================================================================
// Onboard LED (GPIO 2 on most classic ESP32 dev boards). Boards that put their
// LED elsewhere - the ESP32-S3-DevKitC-1 has only an addressable RGB LED, which
// this plain digitalWrite cannot drive - override LED_PIN in config.h.
// ============================================================================
#ifndef LED_PIN
#define LED_PIN 2
#endif
#define LED_BLINK_MS 500  // blink interval when disconnected

// ============================================================================
// Globals
// ============================================================================
// BLE
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pNotifyChar = nullptr;
volatile bool deviceConnected = false;
bool oldDeviceConnected = false;

// The raw advertising payload lives only in the controller - NimBLE keeps no
// copy of custom advertising data and does not re-apply it after a host reset,
// which leaves the ESP32 advertising an empty payload the hood cannot match.
// Re-assert it periodically while disconnected.
#define ADV_REASSERT_MS 30000
volatile unsigned long lastAdvReassert = 0;

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
// Logging
// ============================================================================
// Log through `Log` rather than `Serial`: it writes to the serial console and,
// when remote logging is on, queues the line for MQTT. The queue exists because
// most log lines are written from the NimBLE host task, while PubSubClient may
// only be touched from loop().
#define LOG_LINE_MAX 160
#define LOG_QUEUE_SIZE 24

volatile bool remoteLogEnabled = REMOTE_LOG_DEFAULT;
bool logStateRestored = false;
unsigned long mqttConnectedAt = 0;

static char logLines[LOG_QUEUE_SIZE][LOG_LINE_MAX];
static volatile uint8_t logHead = 0;    // next slot to fill
static volatile uint8_t logTail = 0;    // next slot to publish
static volatile uint16_t logDropped = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

class LogStream : public Print {
public:
  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* data, size_t len) override {
    Serial.write(data, len);
    if (!remoteLogEnabled) return len;

    // A log line is often several write() calls and the lock is released
    // between them, so close the pending line whenever the writer changes.
    // Without this, a status dump from the BLE task and a heap line from
    // loop() interleave into one unreadable line.
    TaskHandle_t writer = xTaskGetCurrentTaskHandle();

    portENTER_CRITICAL(&logMux);
    if (m_partialLen > 0 && writer != m_owner) queueLine();
    m_owner = writer;

    for (size_t i = 0; i < len; i++) {
      char c = (char)data[i];
      // \r ends a line too: progress output that only ever returns the cursor
      // would otherwise fill the buffer until the rest is silently dropped.
      if (c == '\n' || c == '\r') {
        queueLine();
      } else if (m_partialLen < LOG_LINE_MAX - 1) {
        m_partial[m_partialLen++] = c;
      }
    }
    portEXIT_CRITICAL(&logMux);
    return len;
  }

  // Caller holds logMux. Drops a half-written line so switching remote logging
  // back on does not start with the tail of an old one.
  void discardPartial() { m_partialLen = 0; }

private:
  char m_partial[LOG_LINE_MAX];
  size_t m_partialLen = 0;
  TaskHandle_t m_owner = nullptr;

  // Caller holds logMux.
  void queueLine() {
    if (m_partialLen == 0) return;
    m_partial[m_partialLen] = '\0';
    uint8_t next = (logHead + 1) % LOG_QUEUE_SIZE;
    if (next == logTail) {
      logDropped++;
    } else {
      memcpy(logLines[logHead], m_partial, m_partialLen + 1);
      logHead = next;
    }
    m_partialLen = 0;
  }
};

LogStream Log;

// Must not log through Log itself, that would feed the queue it is draining.
void publishPendingLogs() {
  if (!mqtt.connected()) return;

  for (int i = 0; i < LOG_QUEUE_SIZE && logTail != logHead; i++) {
    char line[LOG_LINE_MAX];
    portENTER_CRITICAL(&logMux);
    memcpy(line, logLines[logTail], LOG_LINE_MAX);
    logTail = (logTail + 1) % LOG_QUEUE_SIZE;
    portEXIT_CRITICAL(&logMux);
    mqtt.publish(MQTT_LOG, line);
  }

  if (logDropped > 0) {
    char note[64];
    portENTER_CRITICAL(&logMux);
    uint16_t n = logDropped;
    logDropped = 0;
    portEXIT_CRITICAL(&logMux);
    snprintf(note, sizeof(note), "[LOG] %u lines dropped, queue was full", n);
    mqtt.publish(MQTT_LOG, note);
  }
}

// The state topic is retained and we publish to it ourselves, so stop listening
// once the setting is settled, otherwise our own publish comes back as a command.
void stopLogStateRestore() {
  if (logStateRestored) return;
  logStateRestored = true;
  mqtt.unsubscribe(MQTT_LOG_STATE);
}

void setRemoteLog(bool on) {
  if (!on) {
    // Switching off usually follows something interesting, so send what is
    // still queued before going quiet.
    remoteLogEnabled = false;
    publishPendingLogs();
    portENTER_CRITICAL(&logMux);
    Log.discardPartial();
    portEXIT_CRITICAL(&logMux);
  } else {
    remoteLogEnabled = true;
  }
  if (mqtt.connected()) mqtt.publish(MQTT_LOG_STATE, on ? "ON" : "OFF", true);
}

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

  if (pAdvertising->start()) {
    Log.println("[BLE] Advertising started");
  } else {
    Log.println("[BLE] Advertising start failed, retrying via watchdog");
  }
  lastAdvReassert = millis();
}

// ============================================================================
// GAP Event Logging
// ============================================================================
// The server callbacks carry no reason code, so the raw GAP events are needed
// to tell a lost radio link (supervision timeout) from the hood hanging up on
// purpose, and to see whether a reconnect fails on encryption - a missing key
// means the bond is gone and the hood will not come back until it is paired
// again.
static const char* hciReasonName(int code) {
  switch (code) {
    case BLE_ERR_AUTH_FAIL:          return "authentication failure";
    case BLE_ERR_PINKEY_MISSING:     return "key missing, bond lost";
    case BLE_ERR_CONN_SPVN_TMO:      return "supervision timeout, link lost";
    case BLE_ERR_REM_USER_CONN_TERM: return "closed by the hood";
    case BLE_ERR_CONN_TERM_LOCAL:    return "closed locally";
    case BLE_ERR_CONN_ESTABLISHMENT: return "connection establishment failed";
    default:                         return "see BLE_ERR_* in NimBLE ble.h";
  }
}

// NimBLE folds the error class into the numeric range of the status value.
static void logGapStatus(const char* what, int status) {
  if (status >= BLE_HS_ERR_HCI_BASE && status < BLE_HS_ERR_L2C_BASE) {
    int code = status - BLE_HS_ERR_HCI_BASE;
    Log.printf("[BLE] %s: HCI 0x%02X - %s\n", what, code, hciReasonName(code));
  } else if (status >= BLE_HS_ERR_SM_US_BASE && status < BLE_HS_ERR_SM_PEER_BASE) {
    Log.printf("[BLE] %s: local pairing error 0x%02X\n",
                  what, status - BLE_HS_ERR_SM_US_BASE);
  } else if (status >= BLE_HS_ERR_SM_PEER_BASE && status < BLE_HS_ERR_HW_BASE) {
    Log.printf("[BLE] %s: pairing rejected by the hood, error 0x%02X\n",
                  what, status - BLE_HS_ERR_SM_PEER_BASE);
  } else {
    Log.printf("[BLE] %s: host status %d\n", what, status);
  }
}

static int gapEventLogger(ble_gap_event* event, void* arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISCONNECT:
      logGapStatus("Disconnect", event->disconnect.reason);
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status != 0) {
        logGapStatus("Encryption failed", event->enc_change.status);
      }
      break;
    default:
      break;
  }
  return 0;
}

// ============================================================================
// BLE Callbacks
// ============================================================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) override {
    deviceConnected = true;
    Log.println("[BLE] Hood connected!");
  }

  void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
    Log.printf("[BLE] Peer %s, encrypted=%d bonded=%d\n",
                  NimBLEAddress(desc->peer_ota_addr).toString().c_str(),
                  desc->sec_state.encrypted, desc->sec_state.bonded);
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    Log.printf("[BLE] Authentication complete: encrypted=%d bonded=%d\n",
                  desc->sec_state.encrypted, desc->sec_state.bonded);
  }

  void onDisconnect(NimBLEServer* pServer) override {
    deviceConnected = false;
    Log.println("[BLE] Hood disconnected");
    delay(100);
    startAdvertising();
  }
};

class WriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string value = pChar->getValue();
    Log.printf("[HOOD] Status (%d bytes): ", value.length());
    for (size_t i = 0; i < value.length(); i++) {
      Log.printf("%02X ", (uint8_t)value[i]);
    }
    Log.println();

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
    Log.printf("[BTN] Cannot send %s - not connected\n", name);
    return;
  }

  Log.printf("[BTN] Sending: %s (0x%02X)\n", name, code);

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
    Log.printf("[CMD] Queue full, dropping: %s\n", name);
    return;
  }
  cmdQueue[cmdQueueHead].code = code;
  strncpy(cmdQueue[cmdQueueHead].name, name, sizeof(cmdQueue[cmdQueueHead].name) - 1);
  cmdQueue[cmdQueueHead].name[sizeof(cmdQueue[cmdQueueHead].name) - 1] = '\0';
  cmdQueueHead = next;
  int pending = (cmdQueueHead - cmdQueueTail + CMD_QUEUE_SIZE) % CMD_QUEUE_SIZE;
  Log.printf("[CMD] Queued: %s (0x%02X), pending: %d\n", name, code, pending);
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
#if HOOD_HAS_CEILING_LIGHT
    "\"light_ceiling\":\"%s\","
#endif
    "\"fan_preset\":\"%s\","
    "\"nachlauf\":\"%s\","
#if HOOD_HAS_COVER
    "\"position\":\"%s\","
    "\"cover_state\":\"%s\","
#endif
    "\"ble\":\"%s\","
    "\"status_raw\":\"%02X %02X %02X %02X %02X %02X %02X %02X %02X\""
    "}",
    hood.lightUp ? "ON" : "OFF",
    hood.lightDown ? "ON" : "OFF",
#if HOOD_HAS_CEILING_LIGHT
    hood.lightCeiling ? "ON" : "OFF",
#endif
    berbel::fanPresetName(hood.fanSpeed),
    hood.nachlauf ? "ON" : "OFF",
#if HOOD_HAS_COVER
    hood.position,
    hood.coverState,
#endif
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
#if !HOOD_HAS_CEILING_LIGHT
    // Feature disabled again: drop the entity a previous build registered,
    // otherwise it lingers in HA as a toggle nothing listens to.
    "homeassistant/light/berbel_hood/light_ceiling/config",
#endif
    nullptr
  };
  for (int i = 0; oldTopics[i] != nullptr; i++) {
    mqtt.publish(oldTopics[i], "", true);
    delay(50);
  }
}

void publishDiscovery() {
  Log.println("[MQTT] Publishing HA discovery...");
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

#if HOOD_HAS_CEILING_LIGHT
  // Deckenlicht (ceiling connection with effect lighting)
  publishDiscoveryMsg(
    "homeassistant/light/berbel_hood/light_ceiling/config",
    "\"name\":\"Deckenlicht\","
    "\"uniq_id\":\"berbel_light_ceiling\","
    "\"stat_t\":\"" MQTT_STATE "\","
    "\"cmd_t\":\"" MQTT_CMD_LIGHT_CEILING "\","
    "\"stat_val_tpl\":\"{{ value_json.light_ceiling }}\","
    "\"ic\":\"mdi:led-strip-variant\""
  );
#endif

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

#if HOOD_HAS_COVER
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
#endif

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

  // Remote logging (diagnostic)
  publishDiscoveryMsg(
    "homeassistant/switch/berbel_hood/remote_log/config",
    "\"name\":\"Remote Log\","
    "\"uniq_id\":\"berbel_remote_log\","
    "\"stat_t\":\"" MQTT_LOG_STATE "\","
    "\"cmd_t\":\"" MQTT_CMD_LOG "\","
    "\"ent_cat\":\"diagnostic\","
    "\"ic\":\"mdi:text-box-search-outline\""
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

#if HOOD_HAS_COVER
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
#endif

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
  Log.println("[MQTT] Discovery published!");
}

// ============================================================================
// Restore hood state from retained MQTT message (simple JSON parser)
// ============================================================================
void restoreStateFromMqtt(const char* json) {
  char val[32];

  if (berbel::jsonGetValue(json, "light_up", val, sizeof(val)))
    hood.lightUp = (strcmp(val, "ON") == 0);
  if (berbel::jsonGetValue(json, "light_down", val, sizeof(val)))
    hood.lightDown = (strcmp(val, "ON") == 0);
#if HOOD_HAS_CEILING_LIGHT
  if (berbel::jsonGetValue(json, "light_ceiling", val, sizeof(val)))
    hood.lightCeiling = (strcmp(val, "ON") == 0);
#endif
  if (berbel::jsonGetValue(json, "nachlauf", val, sizeof(val)))
    hood.nachlauf = (strcmp(val, "ON") == 0);
  if (berbel::jsonGetValue(json, "fan_preset", val, sizeof(val)))
    hood.fanSpeed = berbel::fanPresetToSpeed(val);
  if (berbel::jsonGetValue(json, "status_raw", val, sizeof(val)))
    berbel::parseStatusRaw(val, hood.raw);
#if HOOD_HAS_COVER
  if (berbel::jsonGetValue(json, "position", val, sizeof(val)))
    hood.position = (strcmp(val, "Unten") == 0) ? "Unten" : "Oben";
  if (berbel::jsonGetValue(json, "cover_state", val, sizeof(val))) {
    if (strcmp(val, "down") == 0)           hood.coverState = "down";
    else if (strcmp(val, "moving up") == 0) hood.coverState = "moving up";
    else if (strcmp(val, "moving down") == 0) hood.coverState = "moving down";
    else                                   hood.coverState = "up";
  }
#endif

  hoodStateValid = true;
  mqtt.unsubscribe(MQTT_STATE);
#if HOOD_HAS_COVER
  Log.printf("[MQTT] State restored: light_up=%d light_down=%d fan=%d nachlauf=%d pos=%s\n",
    hood.lightUp, hood.lightDown, hood.fanSpeed, hood.nachlauf, hood.position);
#else
  Log.printf("[MQTT] State restored: light_up=%d light_down=%d fan=%d nachlauf=%d\n",
    hood.lightUp, hood.lightDown, hood.fanSpeed, hood.nachlauf);
#endif
}

// ============================================================================
// MQTT Command Callback
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[384];
  unsigned int copyLen = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  memcpy(msg, payload, copyLen);
  msg[copyLen] = '\0';

  Log.printf("[MQTT] %s = %s\n", topic, msg);

  String t(topic);

  // Restore state from retained message on startup
  if (t == MQTT_STATE && !hoodStateValid) {
    restoreStateFromMqtt(msg);
    return;
  }

  // Same for the log switch, so it stays on across the reboot we want logged
  if (t == MQTT_LOG_STATE) {
    if (!logStateRestored) {
      stopLogStateRestore();
      setRemoteLog(strcmp(msg, "ON") == 0);
      Log.printf("[LOG] Remote logging restored: %s\n", remoteLogEnabled ? "on" : "off");
    }
    return;
  }

  // Light Up (Oberlicht) - TOGGLE: check state before sending
  if (t == MQTT_CMD_LIGHT_UP) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.lightUp) {
      Log.printf("[MQTT] Oberlicht already %s, skipping\n", msg);
      return;
    }
    queueButton(BTN_LIGHT_UP, "Light Up");
  }
  // Light Down (Unterlicht) - TOGGLE: check state before sending
  else if (t == MQTT_CMD_LIGHT_DOWN) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.lightDown) {
      Log.printf("[MQTT] Unterlicht already %s, skipping\n", msg);
      return;
    }
    queueButton(BTN_LIGHT_DOWN, "Light Down");
  }
#if HOOD_HAS_CEILING_LIGHT
  // Deckenlicht - TOGGLE: check state before sending
  else if (t == MQTT_CMD_LIGHT_CEILING) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.lightCeiling) {
      Log.printf("[MQTT] Deckenlicht already %s, skipping\n", msg);
      return;
    }
    queueButton(BTN_MULTI, "Light Ceiling");
  }
#endif
  // Power button (Ausschalten)
  else if (t == MQTT_CMD_POWER) {
    queueButton(BTN_POWER, "Power Off");
  }
  // Nachlauf - TOGGLE: check state before sending
  else if (t == MQTT_CMD_NACHLAUF) {
    bool wantOn = (strcmp(msg, "ON") == 0);
    if (wantOn == hood.nachlauf) {
      Log.printf("[MQTT] Nachlauf already %s, skipping\n", msg);
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
      Log.printf("[MQTT] Fan already at %s, skipping\n", msg);
      return;
    }
    queueButton(btnCode, btnName);
  }
#if HOOD_HAS_COVER
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
#endif
  // HA restart - re-publish discovery
  else if (t == "homeassistant/status" && strcmp(msg, "online") == 0) {
    Log.println("[MQTT] HA restarted, re-publishing discovery...");
    publishDiscovery();
    publishState();
  }
  // Remote logging on/off
  else if (t == MQTT_CMD_LOG) {
    stopLogStateRestore();
    setRemoteLog(strcmp(msg, "ON") == 0);
    Log.printf("[LOG] Remote logging %s\n", remoteLogEnabled ? "enabled" : "disabled");
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
  Log.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("berbel-remote");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Log.print(".");
    attempts++;
  }

  // OTA callbacks (set once, begin() called when WiFi is ready)
  ArduinoOTA.setHostname("berbel-remote");
  ArduinoOTA.onStart([]() {
    Log.println("[OTA] Update starting, switching to WiFi priority...");
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  });
  ArduinoOTA.onEnd([]() {
    Log.println("\n[OTA] Update complete, restoring BLE priority...");
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Log.printf("[OTA] %u%%\r", progress * 100 / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Log.printf("[OTA] Error %u\n", error);
  });

  if (WiFi.status() == WL_CONNECTED) {
    Log.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    ArduinoOTA.begin();
    otaReady = true;
    Log.println("[OTA] Ready");
  } else {
    Log.println("\n[WiFi] Connection failed, will retry in loop");
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

  Log.printf("[MQTT] Connecting to %s:%d...\n", MQTT_HOST, MQTT_PORT);

  if (mqtt.connect("berbel-remote", MQTT_USER, MQTT_PASS,
                    MQTT_AVAIL, 0, true, "offline")) {
    Log.println("[MQTT] Connected!");
    mqttConnectedAt = millis();
    mqtt.publish(MQTT_AVAIL, "online", true);

    mqtt.subscribe(MQTT_CMD_LIGHT_UP);
    mqtt.subscribe(MQTT_CMD_LIGHT_DOWN);
#if HOOD_HAS_CEILING_LIGHT
    mqtt.subscribe(MQTT_CMD_LIGHT_CEILING);
#endif
    mqtt.subscribe(MQTT_CMD_POWER);
    mqtt.subscribe(MQTT_CMD_NACHLAUF);
    mqtt.subscribe(MQTT_CMD_FAN_PRESET);
#if HOOD_HAS_COVER
    mqtt.subscribe(MQTT_CMD_POSITION);
    mqtt.subscribe(MQTT_CMD_MOVE_UP);
    mqtt.subscribe(MQTT_CMD_MOVE_DOWN);
#endif
    mqtt.subscribe(MQTT_CMD_DEBUG);
    mqtt.subscribe(MQTT_CMD_LOG);
    mqtt.subscribe("homeassistant/status");

    // Subscribe to the retained log switch state so the setting survives a reboot
    if (!logStateRestored) {
      mqtt.subscribe(MQTT_LOG_STATE);
    }

    // Subscribe to own state topic to restore state from retained message
    if (!hoodStateValid) {
      mqtt.subscribe(MQTT_STATE);
      Log.println("[MQTT] Subscribed to state topic for restore...");
    }

    publishDiscovery();
    publishState();
  } else {
    Log.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
  }
}

// ============================================================================
// Setup
// ============================================================================
void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Serial is the USB CDC here. Once a host has enumerated and then stops
  // draining the FIFO, every write blocks until the timeout - long enough to
  // stall the BLE callbacks that log there. Drop output instead.
  Serial.setTxTimeoutMs(0);
#endif
  delay(1000);

  // LED setup - starts blinking (disconnected state)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Log.println("\n============================================");
  Log.println("  BERBEL REMOTE - HA Bridge (NimBLE)");
  Log.println("============================================\n");

  // ----- BLE Setup (must come first for MAC spoofing) -----

  Log.println("[MAC] Setting Texas Instruments OUI...");
  uint8_t ti_mac[6] = {0x88, 0x01, 0xF9, 0xAA, 0xBB, 0xCC};
  esp_base_mac_addr_set(ti_mac);

  Log.println("[BLE] Initializing NimBLE...");
  NimBLEDevice::init("");

  Log.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());

  // Security: Legacy Pairing, LTK only (no IRK)
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);

  NimBLEDevice::setCustomGapHandler(gapEventLogger);

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
  Log.println("[BLE] Services started, advertising...");

  // Prioritize BLE over WiFi on the shared radio
  esp_coex_preference_set(ESP_COEX_PREFER_BT);

  // WiFi + MQTT - start immediately (NimBLE has enough heap headroom)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);

  Log.printf("[SYS] Free heap before WiFi: %u bytes\n", esp_get_free_heap_size());
  setupWiFi();
  Log.printf("[SYS] Free heap after WiFi: %u bytes\n", esp_get_free_heap_size());
  wifiStarted = true;

  Log.println("\n============================================");
  Log.println("  Ready! Waiting for hood...");
  Log.println("============================================\n");
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
    Log.printf("[SYS] Free heap: %u bytes, BLE: %s, WiFi: %s\n",
      esp_get_free_heap_size(),
      deviceConnected ? "connected" : "waiting",
      wifiStarted ? (WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") : "off");
  }

  // --- OTA ---
  if (wifiStarted && WiFi.status() == WL_CONNECTED) {
    if (!otaReady) {
      ArduinoOTA.begin();
      otaReady = true;
      Log.printf("[OTA] Ready (late init), IP: %s\n", WiFi.localIP().toString().c_str());
    }
    ArduinoOTA.handle();
  }

  // --- WiFi reconnect ---
  if (wifiStarted && WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiRetry = 0;
    if (now - lastWifiRetry > 30000) {
      lastWifiRetry = now;
      Log.println("[WiFi] Reconnecting...");
      WiFi.reconnect();
    }
  }

  // --- MQTT ---
  if (wifiStarted) {
    if (!mqtt.connected()) {
      mqttReconnect();
    } else {
      mqtt.loop();

      // No retained log state showed up in time, so publish ours and stop
      // waiting. Otherwise the HA switch would sit at unknown forever on a
      // device that has never been toggled.
      if (!logStateRestored && now - mqttConnectedAt > 5000) {
        stopLogStateRestore();
        setRemoteLog(remoteLogEnabled);
      }

      publishPendingLogs();
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

  // --- Advertising watchdog ---
  if (!deviceConnected && now - lastAdvReassert > ADV_REASSERT_MS) {
    startAdvertising();
  }

  // --- Process command queue (spaced out button presses) ---
  processCmdQueue();

  // --- Process status update from hood (set in BLE callback) ---
  if (newStatusReceived) {
    newStatusReceived = false;

    // Skip the sync packet (all 0x11) entirely
    if (berbel::isSyncPacket(pendingStatus)) {
      Log.println("[HOOD] Sync packet ignored");
    } else {
      hoodStateValid = true;
      memcpy(hood.raw, pendingStatus, 9);

      berbel::DecodedStatus status = berbel::decodeHoodStatus(hood.raw);
      hood.lightUp   = status.lightUp;
      hood.lightDown = status.lightDown;
#if HOOD_HAS_CEILING_LIGHT
      hood.lightCeiling = status.lightCeiling;
#endif
      hood.fanSpeed  = status.fanSpeed;
      hood.nachlauf  = status.nachlauf;

#if HOOD_HAS_COVER
      berbel::CoverResult cover = berbel::nextCoverState(
        hood.coverState, hood.position, status.movingUp, status.movingDown);
      hood.coverState = cover.state;
      hood.position = cover.position;
#endif

      publishState();
    }
  }

  delay(10);
}
