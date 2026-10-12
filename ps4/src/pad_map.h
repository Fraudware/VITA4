// VITA4 - DualShock 4 -> Vita pad / touch report mapping.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builds the same 28-byte pad reports (core/src/pad_passthrough.c) and
// 16-byte touch records (vita-side pad_wire.h) the PS5 version sends, from a
// platform-neutral snapshot of the PS4 controller. Pure C, host-tested.
#pragma once
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

#include "pad_passthrough.h"

// PS4 scePad button bits (OpenOrbis orbis/_types/pad.h).
#define DS4_L3 0x0002u
#define DS4_R3 0x0004u
#define DS4_OPTIONS 0x0008u
#define DS4_UP 0x0010u
#define DS4_RIGHT 0x0020u
#define DS4_DOWN 0x0040u
#define DS4_LEFT 0x0080u
#define DS4_L2 0x0100u
#define DS4_R2 0x0200u
#define DS4_L1 0x0400u
#define DS4_R1 0x0800u
#define DS4_TRIANGLE 0x1000u
#define DS4_CIRCLE 0x2000u
#define DS4_CROSS 0x4000u
#define DS4_SQUARE 0x8000u
#define DS4_TOUCH_PAD 0x00100000u
#define DS4_INTERCEPTED 0x80000000u // system UI owns the controller

// DualShock 4 touchpad coordinate space reported by scePad.
#define DS4_TOUCH_W 1920
#define DS4_TOUCH_H 942

typedef struct
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; // 0..255, 128 = centre
    uint8_t l2, r2;         // 0..255
    uint8_t fingers;        // 0..2
    uint16_t tx[2], ty[2];  // touchpad positions
    uint8_t connected;
} Ds4State;

enum VdTouchTarget
{
    VD_TOUCH_REAR = 0,  // Vita rear touchpad
    VD_TOUCH_FRONT = 1, // Vita front touchscreen (default, as in VITA5 beta 2026-10-07)
    VD_TOUCH_OFF = 2,
    VD_TOUCH_COUNT
};

// DS4 buttons -> Vita-facing VD_PAD_* mask. Touchpad click is SELECT (the
// DS4 SHARE button belongs to the PS4 system). Intercepted -> 0.
uint32_t vd_ds4_buttons_to_vita(uint32_t ds4_buttons);

// 0..255 (128 centred) -> full-range int16 (0 centred).
int16_t vd_axis_u8_to_i16(uint8_t v);

void vd_ds4_make_report(const Ds4State *s, uint32_t report_id, uint64_t ts_us,
                        VdPadReport *out);

// Neutral report (no buttons, centred sticks) - sent when input stops.
void vd_neutral_report(uint32_t report_id, uint64_t ts_us, VdPadReport *out);

// Touch record for `target` (FRONT/REAR) in the shared VdTouchReport form
// (serialized by core vd_touch_serialize). Active fingers are compacted into
// slots 0/1, inactive slots zeroed, coordinates scaled to the Vita's raw
// 0..1919 x 0..1087 space and clamped. Returns 1, or 0 when touch is off.
int vd_ds4_make_touch(const Ds4State *s, int target, VdTouchReport *out);

// ---- stick dead zone (VITA4 1.39) ---------------------------------------------
// Worn DualShock 4 sticks rest a few units off centre (the user's read 135/132
// at rest): the Vita then sees a constant small push, e.g. a camera that
// turns right faster than left. Readings within VD_STICK_DEADZONE of centre
// (measured as a circle, so diagonals stay smooth) become exactly centre; the
// rest of the range is stretched so full pushes still reach 0 and 255.
#define VD_STICK_DEADZONE 12
void vd_stick_deadzone(uint8_t *x, uint8_t *y);

// ---- touch tap shaping (VITA4 1.20) ------------------------------------------
// Vita games read the touchscreen once per game frame (33 ms at 30 fps) and
// select an option when a touch that landed on it is released there. A
// DS4 tap breaks both: a quick tap can come and go between two game frames
// (nothing happens), and the pad's reading slides as the finger lifts, so
// the game sees a tiny drag and cancels (the option only highlights).
// The shaper turns each physical tap into a clean tap for the game:
//   - every press lasts at least `min_hold` (several game frames);
//   - after a release the Vita sees no touch for at least `gap`, so two quick
//     taps stay two taps (a press that lands sooner waits, it is not lost);
//   - each finger stays exactly where it landed until it has been more than
//     `slop` pad units away for two samples in a row (a lift-off jump is a
//     single sample, so it never turns a tap into a drag); then it follows;
//   - a "no finger" reading shorter than `dropout` mid-press is ignored.
// VD_TAPS_RAW passes the pad through untouched (1.13 / 1.19 behaviour).
enum VdTapMode
{
    VD_TAPS_STEADY = 0, // default
    VD_TAPS_SLOW = 1,   // longer hold, for games that read touch less often
    VD_TAPS_RAW = 2,
    VD_TAPS_COUNT
};
typedef struct
{
    double min_hold, gap, dropout;
    int slop;
} VdTapParams;
void vd_tap_params(int mode, VdTapParams *out);

typedef struct
{
    uint8_t out_fingers;           // what the Vita is being shown
    uint16_t ox[2], oy[2];         // shown position per finger
    uint8_t moving[2], over[2];    // finger follows freely / samples past slop
    double down_since, up_since;   // shown press start / shown release time
    // raw side
    uint8_t raw_fingers;
    uint16_t rx[2], ry[2];
    double raw_seen;               // last sample with a finger
    // a press that landed during the gap, waiting to be shown
    uint8_t pending;
    uint8_t pend_fingers;
    uint16_t px[2], py[2];
} VdTapShaper;

void vd_tap_reset(VdTapShaper *t);
// Updates s->fingers / tx / ty in place for one controller sample at `now`
// (seconds). Mode VD_TAPS_RAW leaves `s` alone (and resets the state).
void vd_tap_apply(VdTapShaper *t, int mode, Ds4State *s, double now);

// Fresh touch id per tap (1.21). A real Vita gives every new finger contact
// a new report id; the Vita plugin labels its two slots with fixed ids
// (0x70 / 0x71). Games that follow touches by id (Killzone Mercenary's pause
// menu) can take a new tap with the same id as the previous one for the old
// touch carrying on - the option highlights but is never pressed. The wire
// record can't carry an id, but it can say which slot a finger is in: each
// new press swaps the slots, so back-to-back taps reach the game as
// different ids. The plugin accepts a lone finger in slot 1 (its decoder
// checks each slot on its own).
typedef struct
{
    uint8_t was_down; // a finger was shown last report
    uint8_t swap;     // this press uses the other slot
} VdTouchAlt;
void vd_touch_alt_reset(VdTouchAlt *a);
// Updates `r` in place; call once per report built (sent or not).
void vd_touch_alternate(VdTouchAlt *a, VdTouchReport *r);

// Release record (count 0) for a port.
void vd_touch_release(int target, VdTouchReport *out);

// 1 if two touch records carry the same state.
int vd_touch_same(const VdTouchReport *a, const VdTouchReport *b);

// 1 if two reports carry the same input state (ignores id/timestamp).
int vd_report_same_input(const VdPadReport *a, const VdPadReport *b);

// L3 chord family (VITA5 beta 2026-10-07-2 pad_bridge chord_events; a Vita
// has no stick clicks, so nothing is lost): L3+D-Pad LEFT/RIGHT cycles the
// upscale mode, L3+D-Pad UP toggles full screen / windowed. Chords fire on the
// rising edge of the D-pad button while L3 is held. (L3+R3, which VITA5 uses
// to toggle input capture, stays VITA4's menu button - main.cpp.)
typedef struct
{
    int upscale_cycle;    // -1 / 0 / +1
    int toggle_fullscreen;
} VdChordEvents;

VdChordEvents vd_chord_events(uint32_t ds4_buttons, uint32_t prev_ds4_buttons);

// What the Vita may see from a raw DS4 mask: while L3 is held the chord
// buttons are the app's (L3/R3/D-pad dropped); otherwise everything passes.
uint32_t vd_mask_modifiers(uint32_t ds4_buttons);

#ifdef __cplusplus
}
#endif
