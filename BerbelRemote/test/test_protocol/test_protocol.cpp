/**
 * Host-side unit tests for the reverse-engineered Berbel protocol logic.
 * Runs in PlatformIO's `native` environment (no ESP32 required):
 *
 *   pio test -e native
 */
#include <unity.h>

#include "berbel_protocol.h"

using namespace berbel;

// ----------------------------------------------------------------------------
// isSyncPacket
// ----------------------------------------------------------------------------
void test_sync_packet_all_0x11(void) {
  uint8_t raw[9] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  TEST_ASSERT_TRUE(isSyncPacket(raw));
}

void test_sync_packet_one_byte_differs(void) {
  uint8_t raw[9] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00};
  TEST_ASSERT_FALSE(isSyncPacket(raw));
}

void test_sync_packet_all_zero_is_not_sync(void) {
  uint8_t raw[9] = {0};
  TEST_ASSERT_FALSE(isSyncPacket(raw));
}

// ----------------------------------------------------------------------------
// decodeHoodStatus - lights
// ----------------------------------------------------------------------------
void test_decode_all_off(void) {
  uint8_t raw[9] = {0};
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_FALSE(s.lightUp);
  TEST_ASSERT_FALSE(s.lightDown);
  TEST_ASSERT_FALSE(s.lightCeiling);
  TEST_ASSERT_EQUAL_UINT8(0, s.fanSpeed);
  TEST_ASSERT_FALSE(s.nachlauf);
  TEST_ASSERT_FALSE(s.movingUp);
  TEST_ASSERT_FALSE(s.movingDown);
}

void test_decode_light_up(void) {
  uint8_t raw[9] = {0};
  raw[2] = 0x10;  // Oberlicht
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_TRUE(s.lightUp);
  TEST_ASSERT_FALSE(s.lightDown);
}

void test_decode_light_down(void) {
  uint8_t raw[9] = {0};
  raw[4] = 0x10;  // Unterlicht
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_FALSE(s.lightUp);
  TEST_ASSERT_TRUE(s.lightDown);
}

void test_decode_light_ceiling(void) {
  uint8_t raw[9] = {0};
  raw[5] = 0x01;  // Deckenlicht
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_FALSE(s.lightUp);
  TEST_ASSERT_FALSE(s.lightDown);
  TEST_ASSERT_TRUE(s.lightCeiling);
}

void test_decode_all_three_lights(void) {
  // Reported in issue #3 from a BFB 6bT (Skyline with lift) that has a ceiling
  // connection, measured with all three lamps on. Confirmed on that hood only.
  uint8_t raw[9] = {0x00, 0x00, 0x10, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00};
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_TRUE(s.lightUp);
  TEST_ASSERT_TRUE(s.lightDown);
  TEST_ASSERT_TRUE(s.lightCeiling);
  TEST_ASSERT_EQUAL_UINT8(0, s.fanSpeed);
  TEST_ASSERT_FALSE(s.nachlauf);
}

void test_decode_ceiling_light_independent_of_nachlauf(void) {
  // raw[5] carries both Deckenlicht (bit 0) and Nachlauf (0x90); they must not
  // interfere: 0x90 | 0x01 = 0x91 -> ceiling light AND afterrun.
  uint8_t raw[9] = {0};
  raw[5] = 0x91;
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_TRUE(s.lightCeiling);
  TEST_ASSERT_TRUE(s.nachlauf);
}

void test_decode_nachlauf_alone_leaves_ceiling_light_off(void) {
  uint8_t raw[9] = {0};
  raw[5] = 0x90;
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_FALSE(s.lightCeiling);
  TEST_ASSERT_TRUE(s.nachlauf);
}

// ----------------------------------------------------------------------------
// decodeHoodStatus - fan speed (priority Power > 3 > 2 > 1 > off)
// ----------------------------------------------------------------------------
void test_decode_fan_stufe_1(void) {
  uint8_t raw[9] = {0};
  raw[0] = 0x10;
  TEST_ASSERT_EQUAL_UINT8(1, decodeHoodStatus(raw).fanSpeed);
}

void test_decode_fan_stufe_2(void) {
  uint8_t raw[9] = {0};
  raw[1] = 0x01;
  TEST_ASSERT_EQUAL_UINT8(2, decodeHoodStatus(raw).fanSpeed);
}

void test_decode_fan_stufe_3(void) {
  uint8_t raw[9] = {0};
  raw[1] = 0x10;
  TEST_ASSERT_EQUAL_UINT8(3, decodeHoodStatus(raw).fanSpeed);
}

void test_decode_fan_power(void) {
  uint8_t raw[9] = {0};
  raw[2] = 0x09;
  TEST_ASSERT_EQUAL_UINT8(4, decodeHoodStatus(raw).fanSpeed);
}

void test_decode_fan_power_takes_priority(void) {
  // Power bits set together with a lower stufe -> Power wins.
  uint8_t raw[9] = {0};
  raw[0] = 0x10;  // would be Stufe 1
  raw[1] = 0x10;  // would be Stufe 3
  raw[2] = 0x09;  // Power
  TEST_ASSERT_EQUAL_UINT8(4, decodeHoodStatus(raw).fanSpeed);
}

// ----------------------------------------------------------------------------
// decodeHoodStatus - nachlauf and cover movement flags
// ----------------------------------------------------------------------------
void test_decode_nachlauf(void) {
  uint8_t raw[9] = {0};
  raw[5] = 0x90;
  TEST_ASSERT_TRUE(decodeHoodStatus(raw).nachlauf);
}

void test_decode_moving_up(void) {
  uint8_t raw[9] = {0};
  raw[4] = 0x01;
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_TRUE(s.movingUp);
  TEST_ASSERT_FALSE(s.movingDown);
}

void test_decode_moving_down(void) {
  uint8_t raw[9] = {0};
  raw[6] = 0x01;
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_FALSE(s.movingUp);
  TEST_ASSERT_TRUE(s.movingDown);
}

void test_decode_light_and_fan_independent(void) {
  // raw[2] carries both Oberlicht (bit4) and Power (bits 0/3); they must not
  // interfere: 0x10 | 0x09 = 0x19 -> light up AND fan power.
  uint8_t raw[9] = {0};
  raw[2] = 0x19;
  DecodedStatus s = decodeHoodStatus(raw);
  TEST_ASSERT_TRUE(s.lightUp);
  TEST_ASSERT_EQUAL_UINT8(4, s.fanSpeed);
}

// ----------------------------------------------------------------------------
// nextCoverState (state machine)
// ----------------------------------------------------------------------------
void test_cover_moving_up_sets_oben(void) {
  CoverResult c = nextCoverState("down", "Unten", true, false);
  TEST_ASSERT_EQUAL_STRING("moving up", c.state);
  TEST_ASSERT_EQUAL_STRING("Oben", c.position);
}

void test_cover_moving_down_sets_unten(void) {
  CoverResult c = nextCoverState("up", "Oben", false, true);
  TEST_ASSERT_EQUAL_STRING("moving down", c.state);
  TEST_ASSERT_EQUAL_STRING("Unten", c.position);
}

void test_cover_settles_up_after_moving_up(void) {
  CoverResult c = nextCoverState("moving up", "Oben", false, false);
  TEST_ASSERT_EQUAL_STRING("up", c.state);
  TEST_ASSERT_EQUAL_STRING("Oben", c.position);
}

void test_cover_settles_down_after_moving_down(void) {
  CoverResult c = nextCoverState("moving down", "Unten", false, false);
  TEST_ASSERT_EQUAL_STRING("down", c.state);
  TEST_ASSERT_EQUAL_STRING("Unten", c.position);
}

// Starting the fan from the parked position sets both flags for the whole
// descent, so the hood is deploying, not retracting.
void test_cover_both_flags_means_deploying(void) {
  CoverResult c = nextCoverState("up", "Oben", true, true);
  TEST_ASSERT_EQUAL_STRING("moving down", c.state);
  TEST_ASSERT_EQUAL_STRING("Unten", c.position);
}

void test_cover_idle_keeps_state(void) {
  CoverResult c = nextCoverState("up", "Oben", false, false);
  TEST_ASSERT_EQUAL_STRING("up", c.state);
  TEST_ASSERT_EQUAL_STRING("Oben", c.position);
}

// ----------------------------------------------------------------------------
// fanPresetName / fanPresetToSpeed (round trip)
// ----------------------------------------------------------------------------
void test_fan_preset_name(void) {
  TEST_ASSERT_EQUAL_STRING("Aus", fanPresetName(0));
  TEST_ASSERT_EQUAL_STRING("Stufe 1", fanPresetName(1));
  TEST_ASSERT_EQUAL_STRING("Stufe 2", fanPresetName(2));
  TEST_ASSERT_EQUAL_STRING("Stufe 3", fanPresetName(3));
  TEST_ASSERT_EQUAL_STRING("Power", fanPresetName(4));
  TEST_ASSERT_EQUAL_STRING("Aus", fanPresetName(99));
}

void test_fan_preset_to_speed(void) {
  TEST_ASSERT_EQUAL_UINT8(0, fanPresetToSpeed("Aus"));
  TEST_ASSERT_EQUAL_UINT8(1, fanPresetToSpeed("Stufe 1"));
  TEST_ASSERT_EQUAL_UINT8(2, fanPresetToSpeed("Stufe 2"));
  TEST_ASSERT_EQUAL_UINT8(3, fanPresetToSpeed("Stufe 3"));
  TEST_ASSERT_EQUAL_UINT8(4, fanPresetToSpeed("Power"));
  TEST_ASSERT_EQUAL_UINT8(0, fanPresetToSpeed("garbage"));
}

void test_fan_preset_round_trip(void) {
  for (uint8_t speed = 0; speed <= 4; speed++) {
    TEST_ASSERT_EQUAL_UINT8(speed, fanPresetToSpeed(fanPresetName(speed)));
  }
}

// ----------------------------------------------------------------------------
// jsonGetValue
// ----------------------------------------------------------------------------
void test_json_get_value_present(void) {
  const char* json = "{\"light_up\":\"ON\",\"fan_preset\":\"Power\"}";
  char val[32];
  TEST_ASSERT_TRUE(jsonGetValue(json, "light_up", val, sizeof(val)));
  TEST_ASSERT_EQUAL_STRING("ON", val);
  TEST_ASSERT_TRUE(jsonGetValue(json, "fan_preset", val, sizeof(val)));
  TEST_ASSERT_EQUAL_STRING("Power", val);
}

void test_json_get_value_missing_key(void) {
  const char* json = "{\"light_up\":\"ON\"}";
  char val[32];
  TEST_ASSERT_FALSE(jsonGetValue(json, "nachlauf", val, sizeof(val)));
}

void test_json_get_value_buffer_too_small(void) {
  const char* json = "{\"x\":\"abcdefghij\"}";
  char val[4];
  TEST_ASSERT_FALSE(jsonGetValue(json, "x", val, sizeof(val)));
}

// ----------------------------------------------------------------------------
// decodeHoodStatus - 13-byte frame (Skyline Edge Play, measured in issue #3)
// ----------------------------------------------------------------------------
void test_long_frame_all_off(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  DecodedStatus s = decodeHoodStatus(raw, STATUS_MAX_LEN);
  TEST_ASSERT_FALSE(s.lightDown);
  TEST_ASSERT_FALSE(s.lightCeiling);
  TEST_ASSERT_EQUAL_UINT8(0, s.fanSpeed);
}

void test_long_frame_light_down(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  raw[4] = 0x10;
  DecodedStatus s = decodeHoodStatus(raw, STATUS_MAX_LEN);
  TEST_ASSERT_TRUE(s.lightDown);
  TEST_ASSERT_FALSE(s.lightCeiling);
}

// The one flag that moved: byte 9 on the long frame, byte 5 on the short one.
void test_long_frame_light_ceiling(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  raw[9] = 0x01;
  DecodedStatus s = decodeHoodStatus(raw, STATUS_MAX_LEN);
  TEST_ASSERT_TRUE(s.lightCeiling);
  TEST_ASSERT_FALSE(s.lightDown);
}

void test_long_frame_ignores_short_frame_ceiling_bit(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  raw[5] = 0x01;
  TEST_ASSERT_FALSE(decodeHoodStatus(raw, STATUS_MAX_LEN).lightCeiling);
}

void test_long_frame_both_lights(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  raw[4] = 0x10;
  raw[9] = 0x01;
  DecodedStatus s = decodeHoodStatus(raw, STATUS_MAX_LEN);
  TEST_ASSERT_TRUE(s.lightDown);
  TEST_ASSERT_TRUE(s.lightCeiling);
}

void test_long_frame_fan_steps(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  raw[0] = 0x10;
  TEST_ASSERT_EQUAL_UINT8(1, decodeHoodStatus(raw, STATUS_MAX_LEN).fanSpeed);

  memset(raw, 0, sizeof(raw));
  raw[1] = 0x01;
  TEST_ASSERT_EQUAL_UINT8(2, decodeHoodStatus(raw, STATUS_MAX_LEN).fanSpeed);

  memset(raw, 0, sizeof(raw));
  raw[1] = 0x10;
  TEST_ASSERT_EQUAL_UINT8(3, decodeHoodStatus(raw, STATUS_MAX_LEN).fanSpeed);
}

// A hood whose frame length we have never seen falls back to the short layout,
// both below and above the lengths we do know.
void test_unknown_frame_length_uses_short_layout(void) {
  uint8_t raw[STATUS_MAX_LEN] = {0};
  raw[5] = 0x01;
  TEST_ASSERT_TRUE(decodeHoodStatus(raw, 11).lightCeiling);
  TEST_ASSERT_TRUE(decodeHoodStatus(raw, 17).lightCeiling);
  raw[5] = 0x00;
  raw[9] = 0x01;
  TEST_ASSERT_FALSE(decodeHoodStatus(raw, 17).lightCeiling);
}

// The sync packet is only ever recognised by its first nine bytes, so a longer
// frame is still a sync packet whatever it carries after them.
void test_sync_packet_ignores_bytes_past_the_ninth(void) {
  uint8_t raw[STATUS_MAX_LEN];
  memset(raw, 0x11, sizeof(raw));
  TEST_ASSERT_TRUE(isSyncPacket(raw));
  raw[12] = 0x00;
  TEST_ASSERT_TRUE(isSyncPacket(raw));
  raw[8] = 0x00;
  TEST_ASSERT_FALSE(isSyncPacket(raw));
}

// ----------------------------------------------------------------------------
// parseStatusRaw
// ----------------------------------------------------------------------------
void test_parse_status_raw_round_trip(void) {
  uint8_t original[9] = {0x00, 0x01, 0x10, 0x09, 0x10, 0x90, 0x01, 0xAB, 0xFF};
  char text[STATUS_MAX_LEN * 3];
  formatStatusRaw(original, 9, text, sizeof(text));

  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_EQUAL_size_t(9, parseStatusRaw(text, parsed, sizeof(parsed)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, parsed, 9);
}

void test_parse_status_raw_round_trip_long_frame(void) {
  uint8_t original[STATUS_MAX_LEN] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
                                      0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
  char text[STATUS_MAX_LEN * 3];
  formatStatusRaw(original, STATUS_MAX_LEN, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("00 00 00 00 10 00 00 00 00 01 00 00 00", text);

  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_EQUAL_size_t(STATUS_MAX_LEN, parseStatusRaw(text, parsed, sizeof(parsed)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, parsed, STATUS_MAX_LEN);
}

void test_parse_status_raw_lowercase(void) {
  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_TRUE(parseStatusRaw("ab cd ef 00 11 22 33 44 55", parsed, sizeof(parsed)));
  TEST_ASSERT_EQUAL_UINT8(0xAB, parsed[0]);
  TEST_ASSERT_EQUAL_UINT8(0xEF, parsed[2]);
}

void test_parse_status_raw_too_few_bytes(void) {
  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_FALSE(parseStatusRaw("00 00 00 00", parsed, sizeof(parsed)));
}

void test_parse_status_raw_too_many_bytes(void) {
  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_FALSE(parseStatusRaw(
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00", parsed, sizeof(parsed)));
}

void test_parse_status_raw_rejects_frame_longer_than_buffer(void) {
  uint8_t parsed[9] = {0};
  TEST_ASSERT_FALSE(parseStatusRaw(
    "00 00 00 00 10 00 00 00 00 01 00 00 00", parsed, sizeof(parsed)));
}

void test_parse_status_raw_non_hex(void) {
  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_FALSE(parseStatusRaw("00 00 ZZ 00 00 00 00 00 00", parsed, sizeof(parsed)));
}

void test_parse_status_raw_leaves_out_untouched_on_failure(void) {
  uint8_t parsed[9] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
  TEST_ASSERT_FALSE(parseStatusRaw("11 22 33 44 55 66 77 88 ZZ", parsed, sizeof(parsed)));
  for (int i = 0; i < 9; i++) TEST_ASSERT_EQUAL_UINT8(0xEE, parsed[i]);
}

void test_parse_status_raw_empty_string(void) {
  uint8_t parsed[STATUS_MAX_LEN] = {0};
  TEST_ASSERT_FALSE(parseStatusRaw("", parsed, sizeof(parsed)));
}

// ----------------------------------------------------------------------------
// Test runner
// ----------------------------------------------------------------------------
int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_sync_packet_all_0x11);
  RUN_TEST(test_sync_packet_one_byte_differs);
  RUN_TEST(test_sync_packet_all_zero_is_not_sync);

  RUN_TEST(test_decode_all_off);
  RUN_TEST(test_decode_light_up);
  RUN_TEST(test_decode_light_down);
  RUN_TEST(test_decode_light_ceiling);
  RUN_TEST(test_decode_all_three_lights);
  RUN_TEST(test_decode_ceiling_light_independent_of_nachlauf);
  RUN_TEST(test_decode_nachlauf_alone_leaves_ceiling_light_off);
  RUN_TEST(test_decode_fan_stufe_1);
  RUN_TEST(test_decode_fan_stufe_2);
  RUN_TEST(test_decode_fan_stufe_3);
  RUN_TEST(test_decode_fan_power);
  RUN_TEST(test_decode_fan_power_takes_priority);
  RUN_TEST(test_decode_nachlauf);
  RUN_TEST(test_decode_moving_up);
  RUN_TEST(test_decode_moving_down);
  RUN_TEST(test_decode_light_and_fan_independent);

  RUN_TEST(test_cover_moving_up_sets_oben);
  RUN_TEST(test_cover_moving_down_sets_unten);
  RUN_TEST(test_cover_settles_up_after_moving_up);
  RUN_TEST(test_cover_settles_down_after_moving_down);
  RUN_TEST(test_cover_both_flags_means_deploying);
  RUN_TEST(test_cover_idle_keeps_state);

  RUN_TEST(test_fan_preset_name);
  RUN_TEST(test_fan_preset_to_speed);
  RUN_TEST(test_fan_preset_round_trip);

  RUN_TEST(test_json_get_value_present);
  RUN_TEST(test_json_get_value_missing_key);
  RUN_TEST(test_json_get_value_buffer_too_small);

  RUN_TEST(test_long_frame_all_off);
  RUN_TEST(test_long_frame_light_down);
  RUN_TEST(test_long_frame_light_ceiling);
  RUN_TEST(test_long_frame_ignores_short_frame_ceiling_bit);
  RUN_TEST(test_long_frame_both_lights);
  RUN_TEST(test_long_frame_fan_steps);
  RUN_TEST(test_unknown_frame_length_uses_short_layout);
  RUN_TEST(test_sync_packet_ignores_bytes_past_the_ninth);

  RUN_TEST(test_parse_status_raw_round_trip);
  RUN_TEST(test_parse_status_raw_round_trip_long_frame);
  RUN_TEST(test_parse_status_raw_lowercase);
  RUN_TEST(test_parse_status_raw_too_few_bytes);
  RUN_TEST(test_parse_status_raw_too_many_bytes);
  RUN_TEST(test_parse_status_raw_rejects_frame_longer_than_buffer);
  RUN_TEST(test_parse_status_raw_non_hex);
  RUN_TEST(test_parse_status_raw_leaves_out_untouched_on_failure);
  RUN_TEST(test_parse_status_raw_empty_string);

  return UNITY_END();
}
