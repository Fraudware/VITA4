// VITA4 - dock a PS Vita to a PS4 over USB: picture, sound and controller.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Threads:
//   main    UI + presentation at the display's 60 Hz (renders the newest frame)
//   link    finds the Vita, runs the stream, reads frames (triple buffered)
//   input   polls the DualShock 4 and forwards pad/touch reports to the Vita
//   audio   plays the Vita's PCM through sceAudioOut
//   usbd    (inside VitaLink) services the isochronous audio transfers
//   upscale (UpscaleWorker, + its row pool) Sharp / FSR upscaling, when on
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <thread>

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_FAILURE_STRINGS
#include "stb_image.h"

#include "audio_ring.h"
#include "canvas.h"
#include "display.h"
#include "goldhen.h"
#include "log.h"
#include "nv12_scale.h"
#include "pacing.h"
#include "pad_map.h"
#include "settings_io.h"
#include "cpu.h"
#include "ui.h"
#include "smooth.h"
#include "upscale_worker.h"
#include "vita_link.h"

using namespace vd4;

namespace
{

constexpr int W = Display::kWidth;
constexpr int H = Display::kHeight;
int g_tv_state = 0; // TV scaler: 0 untried, 1 working, -1 refused by this PS4

// Upscale order with the TV scaler left out once this PS4 has refused it.
int next_upscale(int mode, int dir)
{
    int m = vd_upscale_next(mode, dir);
    if (m == VD_UPSCALE_TV && g_tv_state < 0)
        m = vd_upscale_next(m, dir);
    return m;
}

constexpr const char *kUpscaleMarker = VD4_LOG_DIR "/upscale-running";
constexpr uint32_t kSlotBytes = 2u * 1024u * 1024u; // > 12 + 1280x720 NV12 + slack

void sleep_ms(int ms)
{
    sceKernelUsleep((unsigned)ms * 1000u);
}

// ---- newest-frame exchange between the link thread and the renderer ---------

// Smoothed average for the lag readout (the first sample starts it).
inline float ema(float avg, float sample)
{
    return avg <= 0 ? sample : avg + (sample - avg) * 0.1f;
}

struct FrameSlot
{
    uint8_t *buf = nullptr;
    const uint8_t *nv12 = nullptr;
    int w = 0, h = 0;
    double arrived = 0; // when the last byte came in (lag readout)
};

class FrameExchange
{
  public:
    bool init()
    {
        for (auto &s : slots_)
        {
            s.buf = (uint8_t *)malloc(kSlotBytes);
            if (!s.buf)
                return false;
        }
        return true;
    }
    // Producer.
    FrameSlot &write_slot()
    {
        return slots_[write_];
    }
    void publish()
    {
        write_ = present_.exchange(write_ | kDirty) & 3;
    }
    // A newer frame is waiting (acquire() would switch).
    bool fresh_waiting() const
    {
        return (present_.load() & kDirty) != 0;
    }
    // Consumer: switch to the newest frame if one arrived. Returns true then.
    bool acquire()
    {
        if (!(present_.load() & kDirty))
            return false;
        read_ = present_.exchange(read_) & 3;
        have_ = true;
        return true;
    }
    bool has_frame() const
    {
        return have_;
    }
    const FrameSlot &read_slot() const
    {
        return slots_[read_];
    }

  private:
    static constexpr int kDirty = 4;
    FrameSlot slots_[3];
    std::atomic<int> present_{1};
    int write_ = 0, read_ = 2;
    bool have_ = false;
};

// ---- state shared between threads ------------------------------------------

struct Shared
{
    std::atomic<bool> quit{false};
    std::atomic<int> phase{(int)Phase::starting};
    std::atomic<bool> retry{false};
    std::atomic<bool> restart_stream{false};
    std::atomic<bool> want_audio{true};
    std::atomic<int> audio_state{0};
    std::atomic<bool> pad_enabled{false};
    std::atomic<bool> pad_link{false};
    std::atomic<bool> menu_open{false};
    std::atomic<bool> menu_request{false};
    std::atomic<int> upscale_cycle{0};          // L3 + Left/Right chords, summed
    std::atomic<bool> fullscreen_toggle{false}; // L3 + Up chord
    std::atomic<int> touch_target{VD_TOUCH_FRONT};
    std::atomic<int> tap_mode{VD_TAPS_STEADY};
    std::atomic<bool> deadzone{true};
    std::atomic<bool> passthrough{true};
    std::atomic<uint64_t> input_sent{0};
    std::atomic<int> input_test_request{-1}; // test index to run, -1 none
    std::atomic<bool> input_test_running{false};
    std::atomic<bool> input_test_stop{false};
    std::atomic<bool> input_done{false};     // input thread sent its closing release
    std::mutex test_mutex;
    char input_test_step[VD4_TEXT] = "";
    std::atomic<double> ps_button_until{0};
    std::atomic<double> last_frame_time{0};
    std::atomic<uint32_t> frames{0}, bad_frames{0};
    std::atomic<float> capture_fps{0};
    // Lag readout (Stats), smoothed, milliseconds; 0 = not measured yet.
    std::atomic<float> lag_transfer_ms{0}; // one Vita frame over USB (sceUsbd path)
    std::atomic<float> audio_buffer_ms{0}; // sound waiting to be played
    std::atomic<bool> connected_event{false};
    std::atomic<bool> disconnected_event{false};

    std::mutex text_mutex;
    char status[VD4_TEXT] = "Starting";
    char detail[VD4_TEXT] = "";
    char audio_note[VD4_TEXT] = "";

    std::mutex pad_mutex;
    Ds4State pad{};

    void set_text(const char *s, const char *d)
    {
        std::lock_guard<std::mutex> lock(text_mutex);
        snprintf(status, sizeof(status), "%s", s ? s : "");
        snprintf(detail, sizeof(detail), "%s", d ? d : "");
    }
};

Shared g;
VitaLink g_link;
FrameExchange g_frames;
AudioRing g_ring;
int g_user_id = -1;
int g_pad = -1;

// Sleep in small steps so a retry or quit request wakes the thread early.
void wait_or_retry(int ms)
{
    for (int t = 0; t < ms && !g.quit.load() && !g.retry.load(); t += 50)
        sleep_ms(50);
}

void update_audio(bool force_log)
{
    const bool want = g.want_audio.load();
    if (want && !g_link.audio_running() && g.audio_state.load() != 2)
    {
        if (g_link.start_audio(&g_ring))
        {
            g.audio_state = 1;
        }
        else
        {
            g.audio_state = 2;
            std::lock_guard<std::mutex> lock(g.text_mutex);
            snprintf(g.audio_note, sizeof(g.audio_note), "%s", g_link.audio_problem());
        }
    }
    else if (!want && g_link.audio_running())
    {
        g_link.stop_audio();
        g.audio_state = 0;
    }
    else if (force_log && !want)
    {
        g.audio_state = 0;
    }
}

// ---- link thread -------------------------------------------------------------

void link_thread()
{
    cpu_allow_all();
    // Raw USB fallback: GoldHEN lifts the sandbox so /dev/ugen* can be opened.
    const uint32_t ghen = goldhen_version();
    log_line("GoldHEN SDK version 0x%08x", ghen);
    const int jb = goldhen_jailbreak();
    log_line("GoldHEN jailbreak -> %d", jb);
    char devs[256];
    const int ugen_nodes = survey_dev(devs, sizeof(devs));
    g_link.set_ugen_allowed(jb == 0 || ugen_nodes > 0);

    int rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    log_line("load libSceUsbd module -> 0x%08x", (unsigned)rc);
    rc = g_link.init();
    char last_seen[256] = "-";

    while (!g.quit.load())
    {
        if (rc != 0)
        {
            g.phase = (int)Phase::usb_error;
            char d[VD4_TEXT];
            snprintf(d, sizeof(d),
                     "sceUsbdInit returned 0x%08x. Make sure GoldHEN is running, then press "
                     "Cross to try again.",
                     (unsigned)rc);
            g.set_text("The PS4 USB library didn't start", d);
            while (!g.quit.load() && !g.retry.exchange(false))
                sleep_ms(50);
            if (g.quit.load())
                break;
            rc = g_link.init();
            continue;
        }

        g.phase = (int)Phase::waiting;
        g.set_text("Looking for your Vita", "USB is ready. Nothing that looks like a Vita stream is "
                                             "plugged in yet.");
        g.retry = false;
        char seen[256];
        if (!g_link.find_and_open(seen, sizeof(seen)))
        {
            if (strcmp(seen, last_seen) != 0)
            {
                log_line("USB devices: %s", seen[0] ? seen : "(none)");
                snprintf(last_seen, sizeof(last_seen), "%s", seen);
            }
            wait_or_retry(1000);
            continue;
        }

        g.phase = (int)Phase::connecting;
        g.set_text("Starting the picture", g_link.backend() == Backend::ugen
                                               ? "Vita found through raw USB (GoldHEN)."
                                               : "Vita found through the PS4 USB library.");
        g_link.start_video();
        g.audio_state = 0;
        update_audio(true);
        g.pad_enabled = true;
        g.phase = (int)Phase::live;
        g.connected_event = true;
        g.set_text("Connected", "");

        int errors = 0;
        double window_start = now_seconds(), last_check = now_seconds();
        uint32_t window_frames = 0;
        bool lost = false;
        while (!g.quit.load() && !lost)
        {
            if (g.restart_stream.exchange(false))
            {
                log_line("restarting the stream");
                g_link.stop_video();
                g_link.start_video();
            }
            const bool audio_wanted = g.want_audio.load();
            if (audio_wanted != g_link.audio_running())
            {
                if (!audio_wanted || g.audio_state.load() != 2)
                    update_audio(false);
            }

            FrameSlot &slot = g_frames.write_slot();
            const uint8_t *nv12 = nullptr;
            int w = 0, h = 0;
            const FrameResult r = g_link.read_frame(slot.buf, kSlotBytes, &nv12, &w, &h);
            const double now = now_seconds();
            switch (r)
            {
                case FrameResult::ok:
                    slot.nv12 = nv12;
                    slot.w = w;
                    slot.h = h;
                    slot.arrived = now;
                    g_frames.publish();
                    if (g_link.last_transfer_ms() > 0)
                        g.lag_transfer_ms = ema(g.lag_transfer_ms.load(), g_link.last_transfer_ms());
                    g.frames.fetch_add(1);
                    g.last_frame_time = now;
                    ++window_frames;
                    errors = 0;
                    break;
                case FrameResult::idle:
                    errors = 0;
                    if (now - last_check > 2.0)
                    {
                        last_check = now;
                        if (!g_link.still_present())
                            lost = true;
                        else if (now - g.last_frame_time.load() > 2.0)
                            g_link.recommit(); // VITA5: EP0 data stages are flaky; retry
                    }
                    break;
                case FrameResult::bad:
                    g.bad_frames.fetch_add(1);
                    break;
                case FrameResult::error:
                    ++errors;
                    if (errors == 1 || errors % 10 == 0)
                        log_line("frame read error %d (x%d)", g_link.last_error(), errors);
                    if (g_link.last_error() == VD_USB_ERR_NO_DEVICE || errors >= 3)
                    {
                        if (!g_link.still_present())
                            lost = true;
                        else if (errors >= 10)
                        {
                            g_link.stop_video();
                            g_link.start_video();
                            errors = 0;
                        }
                    }
                    if (!lost)
                        sleep_ms(20);
                    break;
            }
            if (now - window_start >= 1.0)
            {
                g.capture_fps = (float)(window_frames / (now - window_start));
                window_frames = 0;
                window_start = now;
            }
        }

        if (g.quit.load())
        {
            // Let the input thread send its closing release first.
            for (int i = 0; i < 60 && !g.input_done.load(); ++i)
                sleep_ms(5);
        }
        g.pad_enabled = false;
        g.pad_link = false;
        g_link.close();
        g.audio_state = 0;
        g.capture_fps = 0;
        if (lost)
        {
            log_line("the Vita was unplugged");
            g.disconnected_event = true;
        }
    }
    g_link.shutdown();
}

// ---- input thread -------------------------------------------------------------

void read_pad(Ds4State &s)
{
    OrbisPadData d;
    memset(&d, 0, sizeof(d));
    memset(&s, 0, sizeof(s));
    if (g_pad < 0 || scePadReadState(g_pad, &d) != 0)
        return;
    s.connected = d.connected;
    s.buttons = d.buttons;
    s.lx = d.leftStick.x;
    s.ly = d.leftStick.y;
    s.rx = d.rightStick.x;
    s.ry = d.rightStick.y;
    s.l2 = d.analogButtons.l2;
    s.r2 = d.analogButtons.r2;
    s.fingers = d.touch.fingers > 2 ? 2 : d.touch.fingers;
    for (int i = 0; i < 2; ++i)
    {
        s.tx[i] = d.touch.touch[i].x;
        s.ty[i] = d.touch.touch[i].y;
    }
}

// Controller -> Vita, following the VITA5 beta (2026-10-07) bridge policy
// (app/src/vd/pad_bridge.cpp, touch_bridge.cpp):
//   - every change is sent immediately; held input is refreshed every 100 ms
//     (reports replace state on the Vita, which force-resets after 500 ms);
//   - neutral is sent once when input goes idle, on menu open, when
//     forwarding is switched off and on exit - nothing is ever left held;
//   - touch: one record per change, a 250 ms keepalive while a finger is
//     down, a release (count 0) when lifted, and switching the target port
//     releases the old port first.
namespace
{

constexpr double kHoldRefresh = 0.100;
// The Vita plugin holds each report's STICKS for only one input sample
// (input_receiver.c INPUT_ANALOG_MAKE_COUNT 1, ~16.7 ms) while buttons last
// 32 samples. So a stick held still must be re-sent faster than the Vita
// samples, or between refreshes it snaps back to centre: a held right stick
// then turns the camera only a fraction of the time (sluggish).
constexpr double kStickRefresh = 0.012;
// Sticks within this of centre count as resting for the refresh rate.
constexpr int kStickRestI16 = 3 * 256;
constexpr double kMinGap = 0.008; // cap changes at 125 reports/s (7 control OUTs each)
constexpr double kTouchKeepalive = 0.250;

void test_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void test_step(const char *fmt, ...)
{
    char line[VD4_TEXT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lock(g.test_mutex);
        snprintf(g.input_test_step, sizeof(g.input_test_step), "%s", line);
    }
    log_line("input test: %s", line);
}

bool test_wait(double seconds)
{
    const double until = now_seconds() + seconds;
    while (now_seconds() < until)
    {
        if (g.input_test_stop.load() || g.quit.load() || !g.pad_enabled.load())
            return false;
        sleep_ms(5);
    }
    return true;
}

uint32_t g_test_id = 0;

bool test_pad(uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t l2,
              uint8_t r2, double hold)
{
    // Reports latch: repeat at 10 Hz for the hold time (VITA5 pad_hold).
    const int reps = hold <= 0.1 ? 1 : (int)(hold / 0.1);
    for (int i = 0; i < reps; ++i)
    {
        VdPadReport r;
        vd_pad_make_report(&r, ++g_test_id, (uint64_t)(now_seconds() * 1e6), buttons, lx, ly, rx,
                           ry, l2, r2);
        if (g_link.send_pad(r) != 0)
            return false;
        g.input_sent.fetch_add(1);
        if (!test_wait(0.1))
            return false;
    }
    return true;
}

bool test_touch(uint8_t port, uint8_t count, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    VdTouchReport t{};
    t.port = port;
    t.count = count;
    if (count >= 1)
    {
        t.f0_active = 1;
        t.f0_x = x0;
        t.f0_y = y0;
    }
    if (count >= 2)
    {
        t.f1_active = 1;
        t.f1_x = x1;
        t.f1_y = y1;
    }
    return g_link.send_touch(t) == 0;
}

// The VITA5 scripts (screen_inputtest.cpp / payload/padtest.c, touchtest.c,
// touchswipe.c), same steps and timings.
bool run_input_test(int which)
{
    if (which == 0) // LiveArea page swipes: 3 up, 3 down (hardware-proven shape)
    {
        for (int batch = 0; batch < 2; ++batch)
        {
            const int dir = batch == 0 ? 1 : -1;
            for (int n = 1; n <= 3; ++n)
            {
                test_step("swipe %s %d/3", dir > 0 ? "up" : "down", n);
                const int y_from = dir > 0 ? 880 : 180, y_to = dir > 0 ? 180 : 880;
                for (int i = 0; i < 8; ++i)
                {
                    const uint16_t y = (uint16_t)(y_from + ((y_to - y_from) * i) / 7);
                    if (!test_touch(0, 1, 960, y, 0, 0) || !test_wait(0.030))
                        return false;
                }
                if (!test_touch(0, 0, 0, 0, 0, 0) || !test_wait(0.400))
                    return false;
            }
        }
        return true;
    }
    if (which == 1) // button tour
    {
        static const struct
        {
            const char *name;
            uint32_t b;
            int16_t lx, ly, rx, ry;
            uint8_t l2, r2;
        } steps[] = {
            {"D-PAD UP", VD_PAD_UP, 0, 0, 0, 0, 0, 0},
            {"D-PAD DOWN", VD_PAD_DOWN, 0, 0, 0, 0, 0, 0},
            {"D-PAD LEFT", VD_PAD_LEFT, 0, 0, 0, 0, 0, 0},
            {"D-PAD RIGHT", VD_PAD_RIGHT, 0, 0, 0, 0, 0, 0},
            {"SQUARE", VD_PAD_SQUARE, 0, 0, 0, 0, 0, 0},
            {"TRIANGLE", VD_PAD_TRIANGLE, 0, 0, 0, 0, 0, 0},
            {"L1", VD_PAD_L1, 0, 0, 0, 0, 0, 0},
            {"R1", VD_PAD_R1, 0, 0, 0, 0, 0, 0},
            {"L-STICK left", 0, -32768, 0, 0, 0, 0, 0},
            {"L-STICK right", 0, 32767, 0, 0, 0, 0, 0},
            {"R-STICK up", 0, 0, 0, 0, -32768, 0, 0},
            {"R-STICK down", 0, 0, 0, 0, 32767, 0, 0},
            {"neutral (reset)", 0, 0, 0, 0, 0, 0, 0},
        };
        const int n = (int)(sizeof(steps) / sizeof(steps[0]));
        for (int i = 0; i < n; ++i)
        {
            test_step("button %d/%d: %s", i + 1, n, steps[i].name);
            if (!test_pad(steps[i].b, steps[i].lx, steps[i].ly, steps[i].rx, steps[i].ry,
                          steps[i].l2, steps[i].r2, 0.6))
                return false;
        }
        return true;
    }
    if (which == 2 || which == 3) // front tap + swipe / rear swipe
    {
        const uint8_t port = which == 2 ? 0 : 1;
        if (which == 2)
        {
            test_step("front tap at the centre");
            for (int i = 0; i < 6; ++i)
                if (!test_touch(0, 1, 960, 544, 0, 0) || !test_wait(0.1))
                    return false;
            if (!test_touch(0, 0, 0, 0, 0, 0) || !test_wait(0.1))
                return false;
        }
        const uint16_t y = which == 2 ? 400 : 544;
        for (int i = 0; i < 30; ++i)
        {
            test_step("%s swipe %d/30", which == 2 ? "front" : "rear", i + 1);
            const uint16_t x = (uint16_t)(200 + (1500 * i) / 29);
            if (!test_touch(port, 1, x, y, 0, 0) || !test_wait(0.1))
                return false;
        }
        return test_touch(port, 0, 0, 0, 0, 0);
    }
    // Release all.
    test_step("releasing buttons and both touch surfaces");
    return test_pad(0, 0, 0, 0, 0, 0, 0, 0.1) && test_touch(0, 0, 0, 0, 0, 0) &&
           test_touch(1, 0, 0, 0, 0, 0);
}

} // namespace

void input_thread()
{
    cpu_allow_all();
    uint32_t report_id = 0;
    VdPadReport last{};
    bool have_last = false;
    double last_send = 0, last_touch_send = 0;
    VdTouchReport last_touch{};
    bool touch_valid = false; // last_touch describes what the Vita holds
    bool combo_latched = false;
    bool logged_failure = false;
    bool was_blocked = false;
    uint32_t suppress = 0; // buttons held when the menu closed: ignored until released
    uint32_t prev_raw = 0; // last raw DS4 mask (chord edges)

    auto release_all = [&](double now) {
        VdPadReport n;
        vd_neutral_report(++report_id, (uint64_t)(now * 1e6), &n);
        if (g_link.send_pad(n) == 0)
        {
            g.input_sent.fetch_add(1);
            last = n;
            have_last = true;
            last_send = now;
        }
        if (touch_valid && last_touch.count > 0)
        {
            VdTouchReport r;
            vd_touch_release(last_touch.port == 0 ? VD_TOUCH_FRONT : VD_TOUCH_REAR, &r);
            if (g_link.send_touch(r) == 0)
                last_touch = r;
        }
    };

    VdTapShaper taps;
    vd_tap_reset(&taps);
    VdTouchAlt touch_alt;
    vd_touch_alt_reset(&touch_alt);
    while (!g.quit.load())
    {
        Ds4State s;
        read_pad(s);
        {
            std::lock_guard<std::mutex> lock(g.pad_mutex);
            g.pad = s; // raw (Stats shows these)
        }
        // 1.39: stick dead zone (menu, On by default) - only the Vita sees it.
        if (g.deadzone.load())
        {
            vd_stick_deadzone(&s.lx, &s.ly);
            vd_stick_deadzone(&s.rx, &s.ry);
        }
        // Touch taps as games need them (pad_map.h); only the Vita sees this.
        vd_tap_apply(&taps, g.tap_mode.load(), &s, now_seconds());
        const bool live = g.pad_enabled.load() && g.phase.load() == (int)Phase::live;

        // L3 + R3 together opens/closes the menu (the Vita has no L3/R3).
        const bool both = (s.buttons & (DS4_L3 | DS4_R3)) == (DS4_L3 | DS4_R3) &&
                          !(s.buttons & DS4_INTERCEPTED);
        if (both && !combo_latched)
        {
            combo_latched = true;
            if (live)
                g.menu_request = true;
        }
        else if (!(s.buttons & (DS4_L3 | DS4_R3)))
        {
            combo_latched = false;
        }

        // L3 chords (VITA5 beta 2026-10-07-2): upscale cycle / full screen.
        const VdChordEvents chord = vd_chord_events(s.buttons, prev_raw);
        prev_raw = s.buttons;
        if (live && !g.menu_open.load())
        {
            if (chord.upscale_cycle)
                g.upscale_cycle.fetch_add(chord.upscale_cycle);
            if (chord.toggle_fullscreen)
                g.fullscreen_toggle = true;
        }

        if (!live)
        {
            have_last = false;
            touch_valid = false;
            sleep_ms(8);
            continue;
        }

        const double now = now_seconds();

        // Scripted input test (menu): owns the link while it runs.
        const int test = g.input_test_request.exchange(-1);
        if (test >= 0)
        {
            g.input_test_stop = false;
            g.input_test_running = true;
            release_all(now);
            const bool ok = run_input_test(test);
            release_all(now_seconds());
            test_step("%s: %s", ui_input_test_name(test), ok ? "done" : "stopped");
            g.input_test_running = false;
            have_last = false;
            continue;
        }

        const bool forwarding = g.passthrough.load();
        const bool block = g.menu_open.load() || combo_latched || !forwarding;
        if (was_blocked && !block)
            suppress = s.buttons; // e.g. the Circle that closed the menu
        if (!was_blocked && block)
            release_all(now); // nothing stays held while the menu is open / input is off
        was_blocked = block;
        if (block && !forwarding)
        {
            sleep_ms(8);
            continue;
        }
        suppress &= s.buttons;
        Ds4State fwd = s;
        fwd.buttons &= ~suppress;
        fwd.buttons = vd_mask_modifiers(fwd.buttons); // chord buttons stay with VITA4

        VdPadReport rep;
        if (block)
            vd_neutral_report(report_id + 1, (uint64_t)(now * 1e6), &rep);
        else
            vd_ds4_make_report(&fwd, report_id + 1, (uint64_t)(now * 1e6), &rep);
        if (now < g.ps_button_until.load())
            rep.buttons |= VD_PAD_HOME;

        const bool neutral = rep.buttons == 0 && rep.left_x == 0 && rep.left_y == 0 &&
                             rep.right_x == 0 && rep.right_y == 0 && rep.l2 == 0 && rep.r2 == 0;
        const bool changed = !have_last || !vd_report_same_input(&rep, &last);
        const bool stick_held = abs(rep.left_x) > kStickRestI16 || abs(rep.left_y) > kStickRestI16 ||
                                abs(rep.right_x) > kStickRestI16 ||
                                abs(rep.right_y) > kStickRestI16;
        const double refresh = stick_held ? kStickRefresh : kHoldRefresh;
        if ((changed && now - last_send >= kMinGap) ||
            (!neutral && now - last_send >= refresh))
        {
            report_id = rep.report_id;
            const int rc = g_link.send_pad(rep);
            g.pad_link = rc == 0;
            if (rc != 0 && !logged_failure)
            {
                log_line("sending controller input failed: %d", rc);
                logged_failure = true;
            }
            if (rc == 0)
            {
                logged_failure = false;
                g.input_sent.fetch_add(1);
                last = rep; // a failed send stays pending and retries
                have_last = true;
                last_send = now;
            }
        }

        // Touchpad -> Vita touch.
        const int target = g.touch_target.load();
        Ds4State ts = s;
        if (block)
            ts.fingers = 0;
        VdTouchReport rec;
        if (vd_ds4_make_touch(&ts, target, &rec))
        {
            // Steady / Slow games: a fresh touch id for every tap.
            if (g.tap_mode.load() != VD_TAPS_RAW)
                vd_touch_alternate(&touch_alt, &rec);
            else
                vd_touch_alt_reset(&touch_alt);
            // Target switched while a finger was down: release the old port.
            if (touch_valid && last_touch.port != rec.port && last_touch.count > 0)
            {
                VdTouchReport rel;
                vd_touch_release(last_touch.port == 0 ? VD_TOUCH_FRONT : VD_TOUCH_REAR, &rel);
                if (g_link.send_touch(rel) == 0)
                    last_touch = rel;
            }
            const bool idle_unknown = !touch_valid && rec.count == 0;
            const bool changed_t = !touch_valid || !vd_touch_same(&rec, &last_touch);
            const bool keepalive = rec.count > 0 && now - last_touch_send >= kTouchKeepalive;
            if (!idle_unknown && (changed_t || keepalive))
            {
                if (g_link.send_touch(rec) == 0)
                {
                    last_touch = rec;
                    touch_valid = true;
                    last_touch_send = now;
                }
            }
            else if (idle_unknown)
            {
                last_touch = rec; // nothing held anywhere: state known, nothing to send
                touch_valid = true;
            }
        }
        else if (touch_valid && last_touch.count > 0)
        {
            // Touch switched off while a finger was down: release it.
            VdTouchReport rel;
            vd_touch_release(last_touch.port == 0 ? VD_TOUCH_FRONT : VD_TOUCH_REAR, &rel);
            if (g_link.send_touch(rel) == 0)
                last_touch = rel;
        }

        sleep_ms(4);
    }

    // Exit: the closing neutral report + touch release (VITA5 pad_bridge_shutdown).
    if (g.pad_enabled.load())
        release_all(now_seconds());
    g.input_done = true;
}

// ---- audio output thread -------------------------------------------------------

void audio_thread()
{
    cpu_allow_all();
    sceAudioOutInit();
    // Main-port audio belongs to the system user (as in the OpenOrbis audio
    // sample); opening it for the signed-in user fails with 0x809b0001.
    int port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
                               0, 256, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (port <= 0)
    {
        log_line("sceAudioOutOpen (system user) failed: 0x%08x - retrying for user %d",
                 (unsigned)port, g_user_id);
        port = sceAudioOutOpen(g_user_id, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000,
                               ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    }
    if (port <= 0)
    {
        log_line("sceAudioOutOpen failed: 0x%08x", (unsigned)port);
        return;
    }
    log_line("audio output ready (port %d)", port);
    static int16_t grain[256 * 2];
    bool buffering = true;
    uint32_t underruns = 0;
    while (!g.quit.load())
    {
        if (!g_link.audio_running())
        {
            memset(grain, 0, sizeof(grain));
            buffering = true;
            sceAudioOutOutput(port, grain);
            continue;
        }
        // Sound arrives in 32 ms batches (1536 frames), so the backlog swings
        // by that much between batches. 1.32: above 48 ms (more than a batch
        // plus slack) 1 ms is dropped per 5 ms played - too little to hear -
        // instead of letting it reach 85 ms and jumping back to 32 ms. The
        // Vita's sound clock runs a touch fast, so without this the backlog
        // (= the delay) creeps up. Start-up buffering is 1.29's (21 ms).
        const uint32_t avail = g_ring.available();
        g.audio_buffer_ms = ema(g.audio_buffer_ms.load(), (float)avail / 48.0f);
        if (buffering)
        {
            if (avail >= 1024)
                buffering = false;
            else
            {
                memset(grain, 0, sizeof(grain));
                sceAudioOutOutput(port, grain);
                continue;
            }
        }
        if (avail > 2304)
            g_ring.trim(avail - 48);
        const uint32_t got = g_ring.pop(grain, 256);
        if (got < 256)
        {
            memset(grain + got * 2, 0, (256 - got) * 4);
            buffering = true;
            if ((++underruns % 100) == 1)
                log_line("audio underrun (%u)", underruns);
        }
        if (!g.want_audio.load())
            memset(grain, 0, sizeof(grain));
        sceAudioOutOutput(port, grain);
    }
    sceAudioOutClose(port);
}

// ---- rendering helpers ---------------------------------------------------------

struct VideoJob
{
    const uint8_t *nv12;
    const VdScaleMap *map;
    uint32_t *dst;
    uint32_t *scratch[4];
};

void video_rows(void *ctx, int begin, int end, int worker)
{
    VideoJob *j = static_cast<VideoJob *>(ctx);
    vd_nv12_render_rows(j->nv12, j->map, j->dst, W, begin, end, j->scratch[worker & 3]);
}

struct CopyJob
{
    const uint32_t *src;
    uint32_t *dst;
};

void copy_rows(void *ctx, int begin, int end, int)
{
    CopyJob *j = static_cast<CopyJob *>(ctx);
    memcpy(j->dst + (size_t)begin * W, j->src + (size_t)begin * W, (size_t)(end - begin) * W * 4);
}

// Smooth / TV scaler: the Vita picture at its own size (xRGB), then sharpened.
struct SmoothSrcJob
{
    const uint8_t *nv12;
    const VdScaleMap *map; // 1:1
    uint32_t *dst;
    int pitch;
    uint32_t *scratch[4];
};
void smooth_src_rows(void *ctx, int begin, int end, int worker)
{
    SmoothSrcJob *j = static_cast<SmoothSrcJob *>(ctx);
    vd_nv12_render_rows(j->nv12, j->map, j->dst, j->pitch, begin, end, j->scratch[worker & 3]);
}
struct SharpenJob
{
    const uint32_t *src;
    uint32_t *dst;
    int w, h, k;
};
void sharpen_rows(void *ctx, int begin, int end, int)
{
    SharpenJob *j = static_cast<SharpenJob *>(ctx);
    vd_smooth_sharpen_rows(j->src, j->dst, j->w, j->h, j->k, begin, end);
}
struct SmoothJob
{
    const uint32_t *src;
    int w, h;
    uint32_t *dst;
    int row_offset;
    uint32_t *scratch[4];
};
void smooth_rows(void *ctx, int begin, int end, int worker)
{
    SmoothJob *j = static_cast<SmoothJob *>(ctx);
    vd_smooth_render_rows(j->src, j->w, j->h, j->dst, W, j->row_offset, H, begin, end,
                          j->scratch[worker & 3]);
}
// Sharpness setting (0..4) -> unsharp strength k/64 for Smooth / TV scaler.
int smooth_strength(int level)
{
    static const int k[] = {0, 8, 14, 20, 28};
    return k[(level % 5 + 5) % 5];
}

uint32_t *load_image(const char *path)
{
    int w = 0, h = 0, n = 0;
    unsigned char *img = stbi_load(path, &w, &h, &n, 4);
    if (!img || w != W || h != H)
    {
        log_line("%s not loaded (%dx%d)", path, w, h);
        if (img)
            stbi_image_free(img);
        return nullptr;
    }
    uint32_t *px = (uint32_t *)malloc((size_t)W * H * 4);
    if (px)
        for (size_t i = 0; i < (size_t)W * H; ++i)
            px[i] = 0xFF000000u | ((uint32_t)img[i * 4] << 16) | ((uint32_t)img[i * 4 + 1] << 8) |
                    img[i * 4 + 2];
    stbi_image_free(img);
    return px;
}

// Cross-fade: blend the splash image over the canvas with `alpha` (0..256).
struct FadeJob
{
    const uint32_t *src;
    uint32_t *dst;
    uint32_t alpha;
};

void fade_rows(void *ctx, int begin, int end, int)
{
    FadeJob *j = static_cast<FadeJob *>(ctx);
    const uint32_t a = j->alpha, ia = 256 - a;
    for (size_t i = (size_t)begin * W; i < (size_t)end * W; ++i)
    {
        const uint32_t s = j->src[i], d = j->dst[i];
        const uint32_t rb = (((s & 0xFF00FFu) * a + (d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
        const uint32_t g = (((s & 0x00FF00u) * a + (d & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
        j->dst[i] = 0xFF000000u | rb | g;
    }
}

// Logs how much memory the app can still map (PS4 "flexible memory"). The
// OpenOrbis header declares the argument as size_t; the call takes a pointer.
void log_memory(const char *when)
{
    size_t avail = 0;
    auto fn = reinterpret_cast<int32_t (*)(size_t *)>(&sceKernelAvailableFlexibleMemorySize);
    if (fn(&avail) == 0)
        log_line("memory %s: %zu MB free", when, avail >> 20);
}

void toast(UiModel &m, const char *text, double seconds = 2.2)
{
    snprintf(m.toast, sizeof(m.toast), "%s", text);
    m.toast_until = m.time + seconds;
}

void apply_settings(const Settings &s)
{
    g.touch_target = s.touch_target;
    g_link.set_fast_picture(s.fast_picture);
    g.tap_mode = s.taps;
    g.deadzone = s.deadzone;
    g.want_audio = s.sound;
    g.passthrough = s.passthrough;
}

// Menu: change the value of `item` by `dir` (+1 / -1). Returns true if changed.
bool menu_change(UiModel &m, int item, int dir)
{
    Settings &s = m.settings;
    char t[VD4_TEXT];
    switch (item)
    {
        case MENU_PASSTHROUGH:
            s.passthrough = !s.passthrough;
            snprintf(t, sizeof(t), "Controller to Vita: %s", s.passthrough ? "On" : "Off");
            break;
        case MENU_INPUT_TEST:
            if (m.input_test_running)
                return false;
            m.input_test = (m.input_test + dir + VD4_INPUT_TESTS) % VD4_INPUT_TESTS;
            return true; // a choice, not a setting
        case MENU_TOUCH:
            s.touch_target = (s.touch_target + dir + VD_TOUCH_COUNT) % VD_TOUCH_COUNT;
            snprintf(t, sizeof(t), "Touchpad: %s", ui_menu_value(m, MENU_TOUCH));
            break;
        case MENU_TAPS:
            s.taps = (s.taps + dir + VD_TAPS_COUNT) % VD_TAPS_COUNT;
            snprintf(t, sizeof(t), "Touch taps: %s", ui_menu_value(m, MENU_TAPS));
            break;
        case MENU_PICTURE:
            if (s.upscale != VD_UPSCALE_OFF)
            {
                toast(m, "Picture size is set by Upscale while it is on");
                return false;
            }
            s.scale_mode = (s.scale_mode + dir + VD_SCALE_COUNT) % VD_SCALE_COUNT;
            snprintf(t, sizeof(t), "Picture size: %s", ui_menu_value(m, MENU_PICTURE));
            break;
        case MENU_UPSCALE:
            s.upscale = next_upscale(s.upscale, dir);
            snprintf(t, sizeof(t), "Upscale: %s", vd_upscale_label(s.upscale));
            break;
        case MENU_SHARPNESS:
            s.sharpness = (s.sharpness + dir + 5) % 5;
            snprintf(t, sizeof(t), "Sharpness: %s%s", ui_menu_value(m, MENU_SHARPNESS),
                     s.upscale == VD_UPSCALE_OFF ? " (used with Upscale)" : "");
            break;
        case MENU_SOUND:
            if (m.audio_state == 2 && !s.sound)
                return false;
            s.sound = !s.sound;
            if (s.sound && g.audio_state.load() == 2)
                g.audio_state = 0; // allow another attempt
            snprintf(t, sizeof(t), "Sound: %s", s.sound ? "On" : "Off");
            break;
        case MENU_PICSIZE:
            // 1.40: Fast is available again, exactly as in 1.30-1.32 (the
            // 1.37 lock was based on one bad session that may not have been
            // Fast's fault). Note: at 480x272 the Vita sends far more than 60
            // pictures a second, and upscale modes don't apply.
            s.fast_picture = !s.fast_picture;
            g_link.set_fast_picture(s.fast_picture);
            if (g.phase.load() == (int)Phase::live)
                g.restart_stream = true; // the Vita switches size on a fresh start
            snprintf(t, sizeof(t), "Picture resolution: %s", ui_menu_value(m, MENU_PICSIZE));
            break;
        case MENU_DEADZONE:
            s.deadzone = !s.deadzone;
            snprintf(t, sizeof(t), "Stick dead zone: %s", s.deadzone ? "On" : "Off");
            break;
        case MENU_LOW_LAG:
            s.low_lag = !s.low_lag;
            snprintf(t, sizeof(t), "Low lag: %s", s.low_lag ? "On" : "Off");
            break;
        case MENU_STATS:
            s.stats = !s.stats;
            snprintf(t, sizeof(t), "Stats: %s", s.stats ? "On" : "Off");
            break;
        default:
            return false;
    }
    apply_settings(s);
    settings_save(VD4_SETTINGS_PATH, s);
    toast(m, t);
    return true;
}

} // namespace

int main()
{
    sceKernelMkdir(VD4_LOG_DIR, 0777);
    log_open(VD4_LOG_PATH);
    log_line("VITA4 1.0.0 starting");
    log_memory("at start");

    UiModel model;
    ui_init_model(model);
    settings_load(VD4_SETTINGS_PATH, model.settings);
    // Did the last run die while upscaling? Then keep Upscale off this time
    // and show that run's last log lines (triangle on the waiting screen).
    static char prev_log[VD4_LOG_LINES][VD4_TEXT];
    int prev_log_count = 0;
    if (FILE *mark = fopen(kUpscaleMarker, "r"))
    {
        fclose(mark);
        remove(kUpscaleMarker);
        model.prev_crash = true;
        model.settings.upscale = VD_UPSCALE_OFF;
        settings_save(VD4_SETTINGS_PATH, model.settings);
        prev_log_count = log_read_tail(VD4_PREV_LOG_PATH, &prev_log[0][0], VD4_LOG_LINES, VD4_TEXT);
        log_line("the last run stopped while upscaling - Upscale switched off; its last lines:");
        for (int i = 0; i < prev_log_count; ++i)
            log_line("  | %s", prev_log[i]);
    }
    apply_settings(model.settings);
    vd_yuv_tables_init();

    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    sceUserServiceGetInitialUser(&g_user_id);
    if (scePadInit() == 0)
        g_pad = scePadOpen(g_user_id, 0, 0, nullptr);
    log_line("user %d, pad handle %d", g_user_id, g_pad);

    Display display;
    if (!display.init())
    {
        log_line("display init failed - exiting");
        log_close();
        return 1;
    }
    uint32_t *canvas_px = (uint32_t *)malloc((size_t)W * H * 4);
    uint32_t *backdrop = load_image("/app0/assets/backdrop.png");
    uint32_t *splash = load_image("/app0/assets/splash.png");
    if (!canvas_px || !g_frames.init())
    {
        log_line("out of memory");
        return 1;
    }
    Canvas canvas(canvas_px, W, H, W);
    // Spread the work over the six game cores: the main thread on core 0,
    // its render helpers on 1-3; the upscaler (when on) on 1-5.
    log_line("main thread: core %d, core mask 0x%llx", cpu_current(),
             (unsigned long long)cpu_mask_current());
    if (cpu_pin_current(0) != 0)
        log_line("pinning the main thread failed");
    RowPool pool(3, 1);
    static VdScaleMap map, wmap; // full screen / windowed
    map.src_w = 0;
    wmap.src_w = 0;
    // The upscaler (about 41 MB + 5 threads) only exists while Upscale is on.
    UpscaleWorker upscaler;
    int last_upscale = model.settings.upscale;
    uint64_t fb_seq[2] = {0, 0}; // upscaled frame each screen buffer holds (0: none)
    // Smooth / TV scaler: the Vita picture at its own size, plain and sharpened.
    uint32_t *smooth_plain = nullptr, *smooth_sharp = nullptr;
    static VdScaleMap map1; // 1:1
    map1.src_w = 0;
    bool smooth_stale = true; // smooth_sharp needs rebuilding from the current frame
    int smooth_k = -1;
    int &tv_state = g_tv_state;
    log_memory("after setup");
    static uint32_t scratch[4][2048];

    std::thread link(link_thread);
    std::thread input(input_thread);
    std::thread audio(audio_thread);

    const double t0 = now_seconds();
    uint32_t prev_buttons = 0;
    double repeat_at = 0;
    double fps_start = t0, last_tick = t0;
    int fps_frames = 0;
    bool splash_hidden = false;
    // Opening splash: at least 2.5 s, longer while USB is still starting
    // (up to 6 s), then a 0.5 s fade into the app.
    constexpr double kSplashMin = 2.5, kSplashMax = 6.0, kSplashFade = 0.5;
    double splash_fade_start = splash ? -1.0 : 0.0;
    Phase last_phase = Phase::starting;
    // Frame pacing. The screen flips once per refresh (vsync). "Low lag" Off
    // (1.21) draws right after each flip. "Low lag" On (1.29, pacing.h) waits
    // after each flip until a FIXED moment just past where the Vita frames
    // usually arrive, then draws: those frames go out at the very next
    // refresh. The moment is re-chosen from the last 120 arrivals at most
    // every 30 frames and only moves when it is >2 ms off, so frames are
    // shown at an even pace; when no safe moment exists it is 0 (1.21).
    double last_flip = 0, prev_flip = 0, refresh_s = 1.0 / 60.0, draw_peak = 0.008;
    double shown_arrival = 0; // the Vita frame drawn this pass (lag readout)
    double latch = 0;         // Low lag draw moment after the flip (0 = 1.21)
    double arr_phase[120];
    int arr_n = 0, arr_head = 0, arr_new = 0;

    while (!g.quit.load())
    {
        const bool pacing_live = g.phase.load() == (int)Phase::live &&
                                 now_seconds() - g.last_frame_time.load() < 0.5;
        if (!pacing_live)
        {
            arr_n = arr_head = arr_new = 0; // learn afresh when video returns
            latch = 0;
        }
        if (model.settings.low_lag && pacing_live && latch > 0 && last_flip > 0)
        {
            const double target = last_flip + latch;
            while (now_seconds() < target && !g.quit.load())
                sceKernelUsleep(250);
        }
        const double draw_start = now_seconds();
        double submit_at = 0;
        shown_arrival = 0;
        const double now = draw_start;
        model.time = now - t0;

        // ---- pull shared state into the UI model
        model.phase = (Phase)g.phase.load();
        {
            std::lock_guard<std::mutex> lock(g.text_mutex);
            snprintf(model.status, sizeof(model.status), "%s", g.status);
            snprintf(model.detail, sizeof(model.detail), "%s", g.detail);
            snprintf(model.audio_note, sizeof(model.audio_note), "%s", g.audio_note);
        }
        model.audio_state = g.audio_state.load();
        model.pad_link = g.pad_link.load();
        model.input_sent = g.input_sent.load();
        model.input_test_running = g.input_test_running.load();
        {
            std::lock_guard<std::mutex> lock(g.test_mutex);
            snprintf(model.input_test_step, sizeof(model.input_test_step), "%s",
                     g.input_test_step);
        }
        model.frames = g.frames.load();
        model.bad_frames = g.bad_frames.load();
        model.capture_fps = g.capture_fps.load();
        model.lag_transfer_ms = g.lag_transfer_ms.load();
        model.audio_buffer_ms = g.audio_buffer_ms.load();
        const double last_frame = g.last_frame_time.load();
        model.video_signal = model.phase == Phase::live && last_frame > 0 && now - last_frame < 1.0;
        Ds4State pad;
        {
            std::lock_guard<std::mutex> lock(g.pad_mutex);
            pad = g.pad;
        }
        model.controller = pad.connected;
        model.stick_lx = pad.lx;
        model.stick_ly = pad.ly;
        model.stick_rx = pad.rx;
        model.stick_ry = pad.ry;
        if (model.show_log && model.prev_crash)
        {
            memcpy(model.log, prev_log, sizeof(prev_log));
            model.log_count = prev_log_count;
        }
        else if (model.show_log)
            model.log_count = log_snapshot(&model.log[0][0], VD4_LOG_LINES, VD4_TEXT);

        if (model.phase == Phase::live && last_phase != Phase::live)
        {
            model.hint_until = model.time + 7.0;
            model.since_signal = 0;
        }
        if (model.phase != Phase::live)
        {
            model.menu_open = false;
            g.menu_open = false;
        }
        if (g.connected_event.exchange(false))
            toast(model, "Vita connected");
        if (g.disconnected_event.exchange(false))
            toast(model, "Vita disconnected", 3.0);
        if (model.phase == Phase::live && !model.video_signal)
            model.since_signal += 1.0 / 60.0;
        else
            model.since_signal = 0;
        last_phase = model.phase;

        // ---- UI input (edges on the controller the UI owns)
        const uint32_t buttons = (pad.buttons & DS4_INTERCEPTED) ? 0 : pad.buttons;
        uint32_t pressed = buttons & ~prev_buttons;
        // Auto-repeat for menu navigation.
        const uint32_t nav = buttons & (DS4_UP | DS4_DOWN | DS4_LEFT | DS4_RIGHT);
        if (pressed & nav)
            repeat_at = now + 0.4;
        else if (nav && now >= repeat_at)
        {
            pressed |= nav;
            repeat_at = now + 0.12;
        }
        prev_buttons = buttons;
        const bool splash_showing =
            splash && (splash_fade_start < 0 || model.time < splash_fade_start + kSplashFade);
        if (splash_showing)
            pressed = 0; // no accidental Exit while the splash is up

        if (g.menu_request.exchange(false))
        {
            model.menu_open = !model.menu_open;
            model.menu_sel = 0;
            g.menu_open = model.menu_open;
            pressed = 0;
            if (!model.menu_open)
                settings_save(VD4_SETTINGS_PATH, model.settings);
        }

        // L3 chords from the input thread.
        if (const int cyc = g.upscale_cycle.exchange(0))
        {
            Settings &st = model.settings;
            st.upscale = next_upscale(st.upscale, cyc > 0 ? 1 : -1);
            settings_save(VD4_SETTINGS_PATH, st);
            char t[VD4_TEXT];
            snprintf(t, sizeof(t), "Upscale: %s%s", vd_upscale_label(st.upscale),
                     model.windowed && st.upscale ? " (in full screen)" : "");
            toast(model, t);
        }
        if (g.fullscreen_toggle.exchange(false))
        {
            model.windowed = !model.windowed;
            toast(model, model.windowed ? "Windowed" : "Full screen");
        }
        if (model.settings.upscale != last_upscale || model.phase != Phase::live)
        {
            upscaler.reset(); // never show a picture from another mode or stream
            last_upscale = model.settings.upscale;
        }
        if (!vd_upscale_uses_worker(model.settings.upscale) && upscaler.running())
        {
            upscaler.stop(); // give the memory back
            remove(kUpscaleMarker);
            log_line("upscaler stopped");
            log_memory("upscaler off");
        }
        else if (vd_upscale_uses_worker(model.settings.upscale) && !upscaler.running() &&
                 model.phase == Phase::live && !model.windowed)
        {
            log_memory("before upscaler");
            if (FILE *mark = fopen(kUpscaleMarker, "w"))
                fclose(mark); // removed again on a clean stop / exit
            if (upscaler.start(5, 1)) // worker on core 1, helpers on 2-5 and 0
            {
                log_line("upscaler started (%s)", vd_upscale_label(model.settings.upscale));
                log_memory("after upscaler");
            }
            else
            {
                remove(kUpscaleMarker);
                log_line("upscaler: not enough memory - Upscale turned off");
                model.settings.upscale = VD_UPSCALE_OFF;
                last_upscale = VD_UPSCALE_OFF;
                settings_save(VD4_SETTINGS_PATH, model.settings);
                toast(model, "Not enough memory for Upscale - turned it off", 4.0);
            }
        }

        if (model.phase == Phase::live)
        {
            if (model.menu_open)
            {
                if (pressed & DS4_UP)
                    model.menu_sel = (model.menu_sel + MENU_COUNT - 1) % MENU_COUNT;
                if (pressed & DS4_DOWN)
                    model.menu_sel = (model.menu_sel + 1) % MENU_COUNT;
                if (pressed & DS4_LEFT)
                    menu_change(model, model.menu_sel, -1);
                if (pressed & DS4_RIGHT)
                    menu_change(model, model.menu_sel, +1);
                bool close = (pressed & DS4_CIRCLE) != 0;
                if (pressed & DS4_CROSS)
                {
                    switch (model.menu_sel)
                    {
                        case MENU_RESUME:
                            close = true;
                            break;
                        case MENU_PS_BUTTON:
                            g.ps_button_until = now + 0.35;
                            toast(model, "Pressed the Vita's PS button");
                            close = true;
                            break;
                        case MENU_INPUT_TEST:
                            if (!model.input_test_running)
                            {
                                g.input_test_request = model.input_test;
                                char t[VD4_TEXT];
                                snprintf(t, sizeof(t), "Input test: %s - watch the Vita",
                                         ui_input_test_name(model.input_test));
                                toast(model, t, 3.0);
                                close = true;
                            }
                            break;
                        case MENU_RESTART:
                            g.restart_stream = true;
                            toast(model, "Restarting the stream");
                            close = true;
                            break;
                        case MENU_EXIT:
                            g.quit = true;
                            break;
                        default:
                            menu_change(model, model.menu_sel, +1);
                            break;
                    }
                }
                if (close)
                {
                    model.menu_open = false;
                    g.menu_open = false;
                    settings_save(VD4_SETTINGS_PATH, model.settings);
                }
            }
        }
        else
        {
            if (pressed & DS4_TRIANGLE)
                model.show_log = !model.show_log;
            if (pressed & DS4_CROSS)
                g.retry = true;
            if (pressed & DS4_CIRCLE)
                g.quit = true;
        }

        // ---- render
        uint32_t *fb = display.back();
        bool fb_upscaled = false; // this buffer ends up holding fb_seq's picture
        bool tv_presented = false; // the TV scaler's small buffer was flipped instead
        bool smooth_used = false;  // the Smooth / TV path drew this frame
        CopyJob copy{canvas_px, fb};
        if (splash_showing && splash_fade_start < 0 && model.time >= kSplashMin &&
            (model.phase != Phase::starting || model.time >= kSplashMax))
            splash_fade_start = model.time;
        if (splash_showing && splash_fade_start < 0)
        {
            CopyJob from_splash{splash, fb};
            pool.run(copy_rows, &from_splash, H);
        }
        else if (splash_showing)
        {
            // Fade: the screen underneath, with the splash over it.
            if (model.phase == Phase::live)
                ui_draw_no_signal(canvas, model, backdrop);
            else
                ui_draw_screen(canvas, model, backdrop);
            const double left = splash_fade_start + kSplashFade - model.time;
            FadeJob fade{splash, canvas_px, (uint32_t)(256.0 * (left / kSplashFade))};
            pool.run(fade_rows, &fade, H);
            pool.run(copy_rows, &copy, H);
        }
        else if (model.phase == Phase::live)
        {
            const bool fresh = g_frames.acquire();
            model.upscale_active = false;
            if (model.video_signal && g_frames.has_frame())
            {
                const FrameSlot &f = g_frames.read_slot();
                model.video_w = f.w;
                model.video_h = f.h;
                const uint32_t *up = nullptr;
                uint64_t up_seq = 0;
                if (!model.windowed && vd_upscale_uses_worker(model.settings.upscale) &&
                    UpscaleWorker::supported(f.w, f.h))
                {
                    if (fresh)
                        upscaler.submit(f.nv12, f.w, f.h, model.settings.upscale,
                                        model.settings.sharpness);
                    up = upscaler.latest(&up_seq); // null until the first one is done
                }
                model.upscale_active = up != nullptr;
                if (fresh && !up)
                    shown_arrival = f.arrived;
                if (fresh && last_flip > 0 && f.arrived > 0)
                {
                    double ph = -1;
                    if (f.arrived >= last_flip)
                        ph = f.arrived - last_flip;
                    else if (prev_flip > 0 && f.arrived >= prev_flip)
                        ph = f.arrived - prev_flip;
                    if (ph >= 0 && ph < refresh_s)
                    {
                        arr_phase[arr_head] = ph;
                        arr_head = (arr_head + 1) % 120;
                        if (arr_n < 120)
                            ++arr_n;
                        ++arr_new;
                    }
                }
                model.upscale_fps = upscaler.fps();
                model.upscale_ms = upscaler.ms();
                const bool overlay = ui_live_needs_overlay(model);
                const int umode = model.settings.upscale;
                const bool smooth_like = !model.windowed && UpscaleWorker::supported(f.w, f.h) &&
                                         (umode == VD_UPSCALE_SMOOTH || umode == VD_UPSCALE_TV);
                if (smooth_like && !smooth_plain)
                {
                    smooth_plain = (uint32_t *)malloc((size_t)f.w * f.h * 4);
                    smooth_sharp = (uint32_t *)malloc((size_t)f.w * f.h * 4);
                    if (!smooth_plain || !smooth_sharp)
                    {
                        log_line("Smooth: out of memory");
                        model.settings.upscale = VD_UPSCALE_OFF;
                    }
                }
                // TV scaler: tried once, on first use.
                if (smooth_like && umode == VD_UPSCALE_TV && tv_state == 0)
                {
                    const int rc = display.init_small(f.w, f.h);
                    tv_state = rc == 0 ? 1 : -1;
                    if (tv_state < 0)
                    {
                        char t[VD4_TEXT];
                        snprintf(t, sizeof(t), "This PS4 refused the TV scaler (0x%08x) - using Smooth",
                                 (unsigned)rc);
                        toast(model, t, 5.0);
                    }
                }
                if (smooth_like && umode == VD_UPSCALE_TV && tv_state < 0)
                {
                    model.settings.upscale = VD_UPSCALE_SMOOTH;
                    settings_save(VD4_SETTINGS_PATH, model.settings);
                }
                model.tv_scaler = tv_state;
                if (smooth_like && smooth_plain && smooth_sharp)
                {
                    const int k = smooth_strength(model.settings.sharpness);
                    if (fresh || smooth_stale || k != smooth_k)
                    {
                        if (map1.src_w != f.w || map1.src_h != f.h)
                            vd_scale_map_build(&map1, f.w, f.h, f.w, f.h, VD_SCALE_ORIGINAL);
                        SmoothSrcJob sj{f.nv12, &map1, smooth_plain, f.w,
                                        {scratch[0], scratch[1], scratch[2], scratch[3]}};
                        pool.run(smooth_src_rows, &sj, f.h);
                        SharpenJob shj{smooth_plain, smooth_sharp, f.w, f.h, k};
                        pool.run(sharpen_rows, &shj, f.h);
                        smooth_k = k;
                    }
                    const bool new_picture = fresh || smooth_stale;
                    smooth_stale = false;
                    smooth_used = true;
                    model.upscale_active = true;
                    if (umode == VD_UPSCALE_TV && tv_state > 0 && !overlay)
                    {
                        // The PS4's display hardware scales the Vita-size buffer.
                        if (new_picture)
                            memcpy(display.small_back(), smooth_sharp, (size_t)f.w * f.h * 4);
                        submit_at = now_seconds();
                        display.present_small(new_picture);
                        tv_presented = true;
                    }
                    else
                    {
                        SmoothJob job{smooth_sharp, f.w, f.h, overlay ? canvas_px : fb,
                                      (f.h * 2 - H) / 2,
                                      {scratch[0], scratch[1], scratch[2], scratch[3]}};
                        pool.run(smooth_rows, &job, H);
                        if (overlay)
                        {
                            ui_draw_live_overlay(canvas, model);
                            pool.run(copy_rows, &copy, H);
                        }
                    }
                }
                else if (model.windowed)
                {
                    int wx, wy, ww, wh;
                    ui_window_rect(&wx, &wy, &ww, &wh);
                    ui_draw_window_frame(canvas, model);
                    if (wmap.src_w != f.w || wmap.src_h != f.h)
                        vd_scale_map_build(&wmap, f.w, f.h, ww, wh, VD_SCALE_FIT);
                    VideoJob job{f.nv12, &wmap, canvas_px + (size_t)wy * W + wx,
                                 {scratch[0], scratch[1], scratch[2], scratch[3]}};
                    pool.run(video_rows, &job, wh);
                    ui_draw_live_overlay(canvas, model);
                    pool.run(copy_rows, &copy, H);
                }
                else if (up)
                {
                    // Each screen buffer only needs the picture once: skip the
                    // 8 MB copy while it already shows this frame (that time
                    // is left to the upscaler's threads).
                    const int bi = display.back_index();
                    if (overlay)
                    {
                        CopyJob from_up{up, canvas_px};
                        pool.run(copy_rows, &from_up, H);
                        ui_draw_live_overlay(canvas, model);
                        pool.run(copy_rows, &copy, H);
                        fb_seq[bi] = 0;
                    }
                    else
                    {
                        if (fb_seq[bi] != up_seq)
                        {
                            CopyJob from_up{up, fb};
                            pool.run(copy_rows, &from_up, H);
                            fb_seq[bi] = up_seq;
                        }
                        fb_upscaled = true;
                    }
                }
                else
                {
                    if (map.src_w != f.w || map.src_h != f.h ||
                        map.mode != model.settings.scale_mode)
                        vd_scale_map_build(&map, f.w, f.h, W, H, model.settings.scale_mode);
                    VideoJob job{f.nv12, &map, overlay ? canvas_px : fb,
                                 {scratch[0], scratch[1], scratch[2], scratch[3]}};
                    pool.run(video_rows, &job, H);
                    if (overlay)
                    {
                        ui_draw_live_overlay(canvas, model);
                        pool.run(copy_rows, &copy, H);
                    }
                }
            }
            else
            {
                ui_draw_no_signal(canvas, model, backdrop);
                ui_draw_live_overlay(canvas, model);
                pool.run(copy_rows, &copy, H);
            }
        }
        else
        {
            ui_draw_screen(canvas, model, backdrop);
            pool.run(copy_rows, &copy, H);
        }
        if (!smooth_used)
            smooth_stale = true; // another path used this frame: rebuild on return
        if (!tv_presented)
        {
            if (!fb_upscaled)
                fb_seq[display.back_index()] = 0; // drawn by another path
            submit_at = now_seconds();
            display.present();
        }
        {
            const double flip_done = now_seconds();
            if (submit_at > 0)
            {
                const double d = submit_at - draw_start;
                draw_peak = d > draw_peak ? d : draw_peak - 0.00005; // ~3 ms/s decay
                if (draw_peak < 0.002)
                    draw_peak = 0.002;
            }
            const double interval = flip_done - last_flip;
            if (last_flip > 0 && interval > 0.010 && interval < 0.024)
                refresh_s += (interval - refresh_s) * 0.05; // one refresh per pass
            else if (last_flip > 0 && interval >= refresh_s * 1.5 && interval < 0.1 &&
                     model.phase == Phase::live && model.video_signal)
            {
                ++model.late_flips; // this picture missed its refresh
                if (model.settings.low_lag && latch > 0)
                {
                    draw_peak += 0.002; // leave more drawing time from now on
                    arr_new = 30;       // and re-choose right away
                }
            }
            prev_flip = last_flip;
            last_flip = flip_done;
            // Re-choose the Low lag draw moment every 30 arrivals.
            if (arr_new >= 30 && arr_n >= 60)
            {
                arr_new = 0;
                double budget = draw_peak * 1.25 + 0.003;
                budget = budget < 0.005 ? 0.005 : (budget > 0.012 ? 0.012 : budget);
                double tmp[120];
                for (int i = 0; i < arr_n; ++i)
                    tmp[i] = arr_phase[i];
                VdLatchChoice c;
                vd_latch_choose(tmp, arr_n, refresh_s, budget, &c);
                const double want = c.usable ? c.latch : 0.0;
                if (!c.usable || latch == 0 || fabs(want - latch) > 0.002)
                    latch = want;
                model.latch_ms = (float)(latch * 1000.0);
                model.latch_spread_ms = (float)(c.spread * 1000.0);
                model.latch_state = c.usable ? 2 : 3;
            }
            if (!pacing_live)
                model.latch_state = 0;
            else if (model.latch_state == 0)
                model.latch_state = 1; // learning
            if (shown_arrival > 0)
                model.lag_screen_ms =
                    ema(model.lag_screen_ms, (float)((flip_done - shown_arrival) * 1000.0));
        }

        if (splash && !splash_showing)
        {
            free(splash); // the opening splash is done: 8 MB back
            splash = nullptr;
        }
        if (!splash_hidden)
        {
            sceSystemServiceHideSplashScreen();
            splash_hidden = true;
        }
        ++fps_frames;
        if (now - fps_start >= 1.0)
        {
            model.present_fps = (float)(fps_frames / (now - fps_start));
            fps_frames = 0;
            fps_start = now;
        }
        if (model.phase == Phase::live && now - last_tick > 10.0)
        {
            sceSystemServicePowerTick(); // keep the screen from dimming while playing
            last_tick = now;
        }
    }

    log_line("exiting");
    settings_save(VD4_SETTINGS_PATH, model.settings);
    g.quit = true;
    upscaler.stop();
    remove(kUpscaleMarker);
    input.join();
    link.join();
    audio.join();
    log_close();
    sceSystemServiceLoadExec("exit", nullptr);
    return 0;
}
