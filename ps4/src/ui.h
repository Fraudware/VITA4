// VITA4 - screens and overlays (portable; drawn with Canvas).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#include "canvas.h"

namespace vd4
{

enum class Phase
{
    starting,   // USB library coming up
    waiting,    // no Vita on the bus
    connecting, // Vita found, starting the stream
    live,       // stream running (video may still be "no signal")
    usb_error,  // the PS4 USB library would not start
};

enum MenuItem
{
    MENU_RESUME = 0,
    MENU_PASSTHROUGH,
    MENU_TOUCH,
    MENU_TAPS,
    MENU_PICTURE,
    MENU_PICSIZE,
    MENU_UPSCALE,
    MENU_SHARPNESS,
    MENU_SOUND,
    MENU_DEADZONE,
    MENU_LOW_LAG,
    MENU_STATS,
    MENU_PS_BUTTON,
    MENU_INPUT_TEST,
    MENU_RESTART,
    MENU_EXIT,
    MENU_COUNT
};

struct Settings
{
    bool passthrough = true; // controller -> Vita forwarding (VITA5 input_passthrough)
    int touch_target = 1;    // VdTouchTarget: 0 rear / 1 front (default) / 2 off
    int taps = 0;            // VdTapMode: steady (default) / slow games / raw
    int scale_mode = 0;   // VdScaleMode: sharp / fit / original
    int upscale = 0;      // VdUpscaleMode: off (default, as VITA5) / sharp / fsr
    int sharpness = 4;    // RCAS after upscaling: 0 off .. 4 max (FSR 2 slider / 4)
    bool sound = true;
    bool stats = false;
    // Draw each Vita frame as it arrives (1.22/1.23). Off by default since
    // 1.28: with it on, Killzone Mercenary's camera turns stopped and started
    // (frames shown unevenly); Off is 1.21's timing.
    // Low lag (pacing.h). Off by default since 2.0: on tested setups the Vita's
    // frames arrive too irregularly at 960x544 for it to save anything, and
    // Off is the timing confirmed smooth with Fast 480x272.
    bool low_lag = false;
    bool deadzone = true; // 1.39: stick dead zone (vd_stick_deadzone)
    bool fast_picture = false; // 1.30: stream 480x272 instead of 960x544 (less lag)
};

#define VD4_LOG_LINES 18
#define VD4_TEXT 160

struct UiModel
{
    Phase phase = Phase::starting;
    double time = 0;          // seconds since launch (animation clock)
    char status[VD4_TEXT];    // one-line state, e.g. "Looking for your Vita"
    char detail[VD4_TEXT];    // smaller explanation / last error
    bool show_log = false;
    char log[VD4_LOG_LINES][VD4_TEXT];
    int log_count = 0;

    // Live view.
    bool video_signal = false; // frames arriving
    double since_signal = 0;   // seconds without frames (for the placeholder)
    int video_w = 0, video_h = 0;
    float capture_fps = 0, present_fps = 0;
    uint32_t frames = 0, bad_frames = 0;
    int audio_state = 0; // 0 off, 1 playing, 2 unavailable
    char audio_note[VD4_TEXT];
    bool pad_link = false;   // last controller report went out without a USB error
    uint64_t input_sent = 0; // controller reports handed to USB
    int input_test = 0;      // selected scripted test (menu)
    bool input_test_running = false;
    char input_test_step[VD4_TEXT];
    bool controller = false; // DS4 connected
    // 1.38 Stats: raw DualShock 4 stick readings (0..255, 128 = centre).
    int stick_lx = 128, stick_ly = 128, stick_rx = 128, stick_ry = 128;
    double hint_until = 0;   // show the "L3 + R3 menu" hint until this time
    bool prev_crash = false; // last run stopped while upscaling (log view shows that run)
    bool windowed = false;   // L3 + D-Pad Up: picture in a window over the HUD
    bool upscale_active = false; // the upscaled picture is on screen
    int tv_scaler = 0;           // TV scaler test: 0 untried, 1 working, -1 refused
    float upscale_fps = 0;   // upscaled frames finished per second
    float upscale_ms = 0;    // time one upscaled frame takes
    // Lag readout (Stats), ms, 0 = not measured: one Vita frame over USB,
    // Vita frame arrived -> on screen.
    float lag_transfer_ms = 0, lag_screen_ms = 0;
    float audio_buffer_ms = 0; // sound waiting to be played (1.31)
    uint32_t late_flips = 0; // pictures that missed their refresh (judder)
    // Low lag state: 0 no video, 1 learning, 2 drawing at latch_ms after the
    // flip, 3 arrivals too irregular - 1.21 timing. spread = arrival window.
    int latch_state = 0;
    float latch_ms = 0, latch_spread_ms = 0;
    char toast[VD4_TEXT];
    double toast_until = 0;

    bool menu_open = false;
    int menu_sel = 0;
    Settings settings;
};

void ui_init_model(UiModel &m);

// Full-screen screens (waiting, connecting, error). `backdrop` may be null.
void ui_draw_screen(Canvas &c, const UiModel &m, const uint32_t *backdrop);

// Windowed live view: the HUD around a picture rect (ui_window_rect); the
// caller then renders the video into that rect.
void ui_draw_window_frame(Canvas &c, const UiModel &m);
void ui_window_rect(int *x, int *y, int *w, int *h);

// "No signal" placeholder shown while live without frames.
void ui_draw_no_signal(Canvas &c, const UiModel &m, const uint32_t *backdrop);

// True when the live view needs any overlay this frame.
bool ui_live_needs_overlay(const UiModel &m);

// Live overlays (hint, toast, stats, menu) on top of the video.
void ui_draw_live_overlay(Canvas &c, const UiModel &m);

const char *ui_menu_value(const UiModel &m, int item);

#define VD4_INPUT_TESTS 5
const char *ui_input_test_name(int test);

} // namespace vd4
