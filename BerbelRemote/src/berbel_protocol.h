/**
 * Berbel BFB 6bT - pure protocol logic (no Arduino/NimBLE dependencies).
 *
 * Everything here is a side-effect-free function of its inputs so it can be
 * unit-tested on the host with PlatformIO's `native` environment. The decode
 * rules mirror the reverse-engineered protocol documented in main.cpp and
 * REVERSE_ENGINEERING.md.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace berbel {

// Decoded view of a 9-byte hood status notification.
struct DecodedStatus {
  bool lightUp = false;       // Oberlicht
  bool lightDown = false;     // Unterlicht
  bool lightCeiling = false;  // Deckenlicht (ceiling connection, optional accessory)
  uint8_t fanSpeed = 0;       // 0=off, 1-3=Stufe, 4=Power
  bool nachlauf = false;      // afterrun timer active
  bool movingUp = false;      // cover retracting
  bool movingDown = false;    // cover deploying
};

// Hood sends an all-0x11 sync packet on connect that carries no real state.
inline bool isSyncPacket(const uint8_t raw[9]) {
  for (int i = 0; i < 9; i++) {
    if (raw[i] != 0x11) return false;
  }
  return true;
}

// Decode the 9-byte bitmask status into discrete fields.
inline DecodedStatus decodeHoodStatus(const uint8_t raw[9]) {
  DecodedStatus s;

  // Lights (bits don't overlap with fan)
  s.lightUp = (raw[2] & 0x10) != 0;       // Oberlicht: byte 2, bit 4
  s.lightDown = (raw[4] & 0x10) != 0;     // Unterlicht: byte 4, bit 4
  // Deckenlicht: byte 5, bit 0. Shares the byte with Nachlauf (0x90) but no bits
  // overlap. Confirmed on a single hood (issue #3), unverified elsewhere.
  s.lightCeiling = (raw[5] & 0x01) != 0;

  // Fan speed (only one active at a time)
  if (raw[2] & 0x09)      s.fanSpeed = 4;  // Power:   0000 1001
  else if (raw[1] & 0x10) s.fanSpeed = 3;  // Stufe 3: 0001 0000
  else if (raw[1] & 0x01) s.fanSpeed = 2;  // Stufe 2: 0000 0001
  else if (raw[0] & 0x10) s.fanSpeed = 1;  // Stufe 1: 0001 0000
  else                    s.fanSpeed = 0;  // Aus

  s.nachlauf = (raw[5] & 0x90) != 0;       // parallel to fan speed
  s.movingUp = (raw[4] & 0x01) != 0;
  s.movingDown = (raw[6] & 0x01) != 0;
  return s;
}

// Cover state machine result. Strings match the HA entity values.
struct CoverResult {
  const char* state;     // up, moving up, moving down, down
  const char* position;  // Oben, Unten
};

// Next cover state given the previous state and the current moving flags.
// When the hood is not moving, the state settles from "moving up/down" to
// "up/down" and the position is carried over unchanged.
// Deploying wins when both flags are set. A hood that starts from its parked
// position drives down before the fan runs, and reports that with byte 4 and
// byte 6 carrying the same value for the whole ten seconds it takes. Only a
// button-driven move ever sets exactly one of them.
inline CoverResult nextCoverState(const char* prevState, const char* prevPosition,
                                  bool movingUp, bool movingDown) {
  if (movingDown) return {"moving down", "Unten"};
  if (movingUp)   return {"moving up", "Oben"};
  if (strcmp(prevState, "moving up") == 0)   return {"up", prevPosition};
  if (strcmp(prevState, "moving down") == 0) return {"down", prevPosition};
  return {prevState, prevPosition};
}

// Human-readable fan preset label for a given speed.
inline const char* fanPresetName(uint8_t speed) {
  switch (speed) {
    case 1: return "Stufe 1";
    case 2: return "Stufe 2";
    case 3: return "Stufe 3";
    case 4: return "Power";
    default: return "Aus";
  }
}

// Inverse of fanPresetName: parse a preset label back into a speed.
// Unknown labels (including "Aus") map to 0 (off).
inline uint8_t fanPresetToSpeed(const char* preset) {
  if (strcmp(preset, "Stufe 1") == 0) return 1;
  if (strcmp(preset, "Stufe 2") == 0) return 2;
  if (strcmp(preset, "Stufe 3") == 0) return 3;
  if (strcmp(preset, "Power") == 0)   return 4;
  return 0;
}

// Parse a `berbel/hood/debug/send` payload: a two-digit hex button code, and
// optionally `:` plus how long to hold the button in milliseconds. "0B" and
// "0B:2000" are both valid, anything else is rejected rather than guessed at,
// since the whole point of the topic is careful probing.
// `code` and `holdMs` are only written on success. `holdMs` is left at whatever
// the caller put there when the payload carries no hold time.
inline bool parseDebugCommand(const char* s, uint16_t maxHoldMs,
                              uint8_t* code, uint16_t* holdMs) {
  char* end = nullptr;
  long parsedCode = strtol(s, &end, 16);
  if (end == s || parsedCode < 0x01 || parsedCode > 0xFF) return false;

  long parsedHold = -1;
  if (*end == ':') {
    const char* holdStart = end + 1;
    parsedHold = strtol(holdStart, &end, 10);
    if (end == holdStart || parsedHold < 1 || parsedHold > maxHoldMs) return false;
  }
  if (*end != '\0') return false;

  *code = (uint8_t)parsedCode;
  if (parsedHold >= 0) *holdMs = (uint16_t)parsedHold;
  return true;
}

// Extract a string value for `key` from a flat `"key":"value"` JSON object.
// Returns false if the key is missing or the value doesn't fit in `out`.
inline bool jsonGetValue(const char* json, const char* key, char* out, size_t outLen) {
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

// Parse the space-separated 9-byte hex string used for `status_raw`
// (e.g. "00 00 00 00 10 00 00 00 00") back into raw bytes. `out` is only
// written on success. Returns false unless the string holds exactly 9
// two-digit hex bytes separated by single spaces.
inline bool parseStatusRaw(const char* s, uint8_t out[9]) {
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };

  uint8_t parsed[9];
  const char* p = s;
  for (int i = 0; i < 9; i++) {
    if (i > 0 && *p++ != ' ') return false;
    int hi = hexVal(p[0]);
    if (hi < 0) return false;
    int lo = hexVal(p[1]);
    if (lo < 0) return false;
    parsed[i] = (uint8_t)((hi << 4) | lo);
    p += 2;
  }
  if (*p != '\0') return false;

  memcpy(out, parsed, 9);
  return true;
}

}  // namespace berbel
