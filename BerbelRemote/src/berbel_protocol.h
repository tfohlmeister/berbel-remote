/**
 * Berbel hood - pure protocol logic (no Arduino/NimBLE dependencies).
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

// Decoded view of a hood status notification.
struct DecodedStatus {
  bool lightUp = false;       // Oberlicht
  bool lightDown = false;     // Unterlicht
  bool lightCeiling = false;  // Deckenlicht (ceiling connection, optional accessory)
  uint8_t fanSpeed = 0;       // 0=off, 1-3=Stufe, 4=Power
  bool nachlauf = false;      // afterrun timer active
  bool movingUp = false;      // cover retracting
  bool movingDown = false;    // cover deploying
};

// Longest status frame known. The BFB 6bT sends 9 bytes, the Skyline Edge Play
// 13. Frames are truncated to this length, anything beyond stays undecoded.
static const size_t STATUS_MAX_LEN = 13;

// One flag in the status frame: which byte carries it, and which bits to test.
struct StatusBit {
  uint8_t index;
  uint8_t mask;
};

// Where each flag sits in the frame. Hood models disagree on this and the frame
// length is the only thing that tells them apart, so every field is looked up in
// a layout rather than read from a fixed offset. A model that moves another flag
// is one more table below, not another branch in the decoder.
struct StatusLayout {
  StatusBit fanStep1;
  StatusBit fanStep2;
  StatusBit fanStep3;
  StatusBit fanPower;
  StatusBit lightUp;
  StatusBit lightDown;
  StatusBit lightCeiling;
  StatusBit nachlauf;
  StatusBit movingUp;
  StatusBit movingDown;
};

// 9-byte frame (BFB 6bT). Every field measured.
static const StatusLayout LAYOUT_SHORT = {
  /* fanStep1     */ {0, 0x10},
  /* fanStep2     */ {1, 0x01},
  /* fanStep3     */ {1, 0x10},
  /* fanPower     */ {2, 0x09},
  /* lightUp      */ {2, 0x10},
  /* lightDown    */ {4, 0x10},
  /* lightCeiling */ {5, 0x01},
  /* nachlauf     */ {5, 0x90},
  /* movingUp     */ {4, 0x01},
  /* movingDown   */ {6, 0x01},
};

// 13-byte frame (Skyline Edge Play). Everything except Fan Power is confirmed on
// such a hood: the fan steps, both lights, the lift flags and the Deckenlicht on
// byte 9 come from issue #3, and berbel-ha, an independent integration for the
// same hoods, decodes the lights, fan steps and Nachlauf at the same offsets.
// Fan Power is carried over from the short layout and remains unverified.
static const StatusLayout LAYOUT_LONG = {
  /* fanStep1     */ {0, 0x10},
  /* fanStep2     */ {1, 0x01},
  /* fanStep3     */ {1, 0x10},
  /* fanPower     */ {2, 0x09},
  /* lightUp      */ {2, 0x10},
  /* lightDown    */ {4, 0x10},
  /* lightCeiling */ {9, 0x01},
  /* nachlauf     */ {5, 0x90},
  /* movingUp     */ {4, 0x01},
  /* movingDown   */ {6, 0x01},
};

// Pick the layout for a frame of `len` bytes. Only a length we have measured gets
// its own layout; everything else falls back to the short one, whose indices stay
// within the nine bytes every hood agrees on.
inline StatusLayout statusLayout(size_t len) {
  return len == STATUS_MAX_LEN ? LAYOUT_LONG : LAYOUT_SHORT;
}

inline bool statusBitSet(const uint8_t* raw, StatusBit bit) {
  return (raw[bit.index] & bit.mask) != 0;
}

// Hood sends an all-0x11 sync packet on connect that carries no real state.
// Only the first nine bytes are checked: that is the whole packet on a 9-byte
// hood, and what a longer frame carries after them has never been measured.
// Requiring 0x11 there too would risk taking a sync packet for real state and
// publishing it retained over the last known good one.
inline bool isSyncPacket(const uint8_t raw[9]) {
  for (size_t i = 0; i < 9; i++) {
    if (raw[i] != 0x11) return false;
  }
  return true;
}

// Decode the bitmask status into discrete fields, using the layout that matches
// the frame length.
inline DecodedStatus decodeHoodStatus(const uint8_t* raw, size_t len) {
  const StatusLayout l = statusLayout(len);
  DecodedStatus s;

  s.lightUp = statusBitSet(raw, l.lightUp);
  s.lightDown = statusBitSet(raw, l.lightDown);
  s.lightCeiling = statusBitSet(raw, l.lightCeiling);

  // Fan speed (only one active at a time)
  if (statusBitSet(raw, l.fanPower))       s.fanSpeed = 4;
  else if (statusBitSet(raw, l.fanStep3))  s.fanSpeed = 3;
  else if (statusBitSet(raw, l.fanStep2))  s.fanSpeed = 2;
  else if (statusBitSet(raw, l.fanStep1))  s.fanSpeed = 1;
  else                                     s.fanSpeed = 0;

  s.nachlauf = statusBitSet(raw, l.nachlauf);  // parallel to fan speed
  s.movingUp = statusBitSet(raw, l.movingUp);
  s.movingDown = statusBitSet(raw, l.movingDown);
  return s;
}

inline DecodedStatus decodeHoodStatus(const uint8_t raw[9]) {
  return decodeHoodStatus(raw, 9);
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

// Render `len` bytes as the space-separated hex string used for `status_raw`
// (e.g. "00 00 00 00 10 00 00 00 00"). Needs `len * 3` bytes of room in `out`.
inline void formatStatusRaw(const uint8_t* raw, size_t len, char* out, size_t outLen) {
  if (outLen == 0) return;
  size_t pos = 0;
  for (size_t i = 0; i < len && pos + 3 < outLen; i++) {
    pos += snprintf(out + pos, outLen - pos, i == 0 ? "%02X" : " %02X", raw[i]);
  }
  out[pos] = '\0';
}

// Parse the space-separated hex string used for `status_raw` back into bytes.
// Returns the number of bytes parsed, 0 if the string is malformed, holds fewer
// than 9 bytes or more than fit. `out` is only written on success.
inline size_t parseStatusRaw(const char* s, uint8_t* out, size_t outLen) {
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };

  uint8_t parsed[STATUS_MAX_LEN];
  size_t count = 0;
  const char* p = s;
  while (*p != '\0') {
    if (count > 0 && *p++ != ' ') return 0;
    if (count >= STATUS_MAX_LEN || count >= outLen) return 0;
    int hi = hexVal(p[0]);
    if (hi < 0) return 0;
    int lo = hexVal(p[1]);
    if (lo < 0) return 0;
    parsed[count++] = (uint8_t)((hi << 4) | lo);
    p += 2;
  }
  if (count < 9) return 0;

  memcpy(out, parsed, count);
  return count;
}

}  // namespace berbel
