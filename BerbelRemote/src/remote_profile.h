/**
 * Which Berbel remote this firmware emulates.
 *
 * Three remotes reach Berbel hoods, and they do not share one vocabulary:
 *
 *   BFB 6bT (Art. 1090045)  13 keys, codes 0x01-0x0D, discontinued
 *   BFB 8bT (Art. 1090093)  13 keys, the same codes, the 6bT's replacement
 *   BFB 7bT (Art. 1090084)  19 keys, 0x01-0x0E and 0x12-0x17, no 0x07,
 *                           shipped with the Skyline Edge Base and Play
 *
 * The 6bT and 8bT were each read out key by key and emit an identical set, so
 * they share a profile here. The 7bT adds seven codes of its own, and those are
 * what a profile has to switch on: they are the only thing about it this
 * firmware cannot already do.
 *
 * Select with REMOTE_PROFILE in config.h. Codes and how they were measured are
 * in REVERSE_ENGINEERING.md.
 *
 * ---------------------------------------------------------------------------
 * What a profile deliberately does NOT change: the identity this firmware
 * presents to the hood. Spoofed MAC, Device Information strings and GATT device
 * name stay the same for every profile, although the real remotes differ in all
 * three (a 7bT calls itself "Hood BLE v7", an 8bT "Hood BLE v6.1", and each
 * carries its own Texas Instruments OUI).
 *
 * The reason is evidence. Skyline Edge owners already run this firmware with
 * the identity below and reach their hoods, so it is known to work where a 7bT
 * would be the original. The identities of the 8bT and 7bT, by contrast, were
 * read off remotes that were never paired to a hood here. Switching to them
 * would trade a proven value for an unproven one, and the only way to find out
 * would be a user whose hood then stops pairing.
 * ---------------------------------------------------------------------------
 */
#pragma once

#define REMOTE_BFB_6BT 6
#define REMOTE_BFB_8BT 8
#define REMOTE_BFB_7BT 7

// Emulating a 6bT is what this firmware has always done, so that is the default
// and an existing config.h keeps building unchanged.
#ifndef REMOTE_PROFILE
#define REMOTE_PROFILE REMOTE_BFB_6BT
#endif

#if REMOTE_PROFILE == REMOTE_BFB_6BT || REMOTE_PROFILE == REMOTE_BFB_8BT

#if REMOTE_PROFILE == REMOTE_BFB_6BT
#define REMOTE_NAME "BFB 6bT"
#else
#define REMOTE_NAME "BFB 8bT"
#endif

// Colour temperature, Motion-Lights and a dedicated Uplight key exist only on
// the 7bT. Neither of these two remotes has them.
#define REMOTE_HAS_COLOUR_TEMP   0
#define REMOTE_HAS_MOTION_LIGHTS 0
#define REMOTE_HAS_UPLIGHT       0

#elif REMOTE_PROFILE == REMOTE_BFB_7BT

#define REMOTE_NAME "BFB 7bT"
#define REMOTE_HAS_COLOUR_TEMP   1
#define REMOTE_HAS_MOTION_LIGHTS 1
#define REMOTE_HAS_UPLIGHT       1

#else
#error "REMOTE_PROFILE must be REMOTE_BFB_6BT, REMOTE_BFB_8BT or REMOTE_BFB_7BT"
#endif

// ---------------------------------------------------------------------------
// The 7bT's own button codes.
//
// Defined for every profile so the debug topic can reach them on any build:
// a code the remote does not have is one the hood may still act on, and finding
// that out is exactly what `berbel/hood/debug/send` is for. Only the entities
// are gated on the profile.
//
// None of these has been driven at a hood. They were read off a BFB 7bT and
// matched to its manual, which is a statement about the remote, not about what
// a hood does with them.
// ---------------------------------------------------------------------------
#define BTN_NO_FUNCTION      0x0E  // the "A" key, which the manual lists as unassigned
#define BTN_UPLIGHT          0x12  // Uplight (Skyline Edge Play); this firmware's Deckenlicht
#define BTN_TEMP_UPLIGHT     0x13  // colour temperature of the Uplight
#define BTN_TEMP_LIGHT_DOWN  0x14  // colour temperature of the Kochfeld-Beleuchtung
#define BTN_TEMP_LIGHT_UP    0x15  // colour temperature of the Effektbeleuchtung
#define BTN_MOTION_LIGHTS    0x16  // Motion-Lights on/off
#define BTN_MOTION_NEXT      0x17  // Motion-Lights, step to the next one
