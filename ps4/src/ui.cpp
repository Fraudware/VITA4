// VITA4 - screens and overlays. See ui.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "font_data.h"
#include "upscale.h"

namespace vd4
{

namespace
{

// Palette shared with the PS5 build (app/src/vd_theme.hpp).
constexpr uint32_t kPanel = 0x111c33;
constexpr uint32_t kPanelHigh = 0x1a2a47;
constexpr uint32_t kInk = 0xeaf2ff;
constexpr uint32_t kMuted = 0x8fa6c8;
constexpr uint32_t kSignal = 0x2fe0c9;
constexpr uint32_t kWaiting = 0xffb347;
constexpr uint32_t kAlert = 0xff5d6c;
constexpr uint32_t kOutline = 0x2a3c5e;

constexpr int kMargin = 96;

float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

int fade_alpha(double now, double until, double fade = 0.4)
{
    const double left = until - now;
    if (left <= 0)
        return 0;
    return (int)(255.0 * clamp01((float)(left / fade)));
}

void backdrop_or_fill(Canvas &c, const uint32_t *backdrop)
{
    if (backdrop)
        c.copy_from(backdrop, c.width());
    else
        c.vgradient(0, 0, c.width(), c.height(), 0x050a14, 0x1a1240);
}

void draw_brand(Canvas &c, int x, int top, const Font &f)
{
    int pen = x;
    pen += c.text(f, pen, top, "vita", kInk);
    pen += c.text(f, pen, top, "Dock", kSignal);
    const int size = f.height - 4;
    const int bx = pen + 12, by = top + 4;
    c.round_rect((float)bx, (float)by, (float)size, (float)size, 10, kSignal, 46);
    c.text_centered(f, bx + size / 2, top, "4", kSignal);
}

void draw_pill(Canvas &c, int right, int top, const char *label, uint32_t dot, float breathe)
{
    const Font &f = font_label;
    const int tw = Canvas::measure(f, label);
    const int h = 58, pad = 26, dot_r = 8;
    const int w = pad + dot_r * 2 + 16 + tw + pad;
    const int x = right - w;
    c.round_rect((float)x, (float)top, (float)w, (float)h, h * 0.5f, kPanel, 215);
    c.round_rect_outline((float)x, (float)top, (float)w, (float)h, h * 0.5f, 1.5f, kOutline, 255);
    const float cx = (float)(x + pad + dot_r), cy = top + h * 0.5f;
    c.circle(cx, cy, dot_r + 6 * breathe, dot, (int)(60 * (1 - breathe)));
    c.circle(cx, cy, (float)dot_r, dot);
    c.text(f, x + pad + dot_r * 2 + 16, top + (h - f.height) / 2, label, kInk);
}

void phase_pill(const UiModel &m, const char **label, uint32_t *color)
{
    switch (m.phase)
    {
        case Phase::starting:
            *label = "Starting";
            *color = kWaiting;
            break;
        case Phase::waiting:
            *label = "Waiting for Vita";
            *color = kWaiting;
            break;
        case Phase::connecting:
            *label = "Connecting";
            *color = kSignal;
            break;
        case Phase::live:
            *label = m.video_signal ? "Live" : "Connected";
            *color = kSignal;
            break;
        case Phase::usb_error:
            *label = "USB unavailable";
            *color = kAlert;
            break;
    }
}

void draw_hint(Canvas &c, int x, int top, FaceButton b, const char *label, uint32_t color = kInk)
{
    draw_face_button(c, b, (float)(x + 22), (float)(top + 22), 44);
    c.text(font_label, x + 56, top + 22 - font_label.height / 2, label, color);
}

int hint_width(const char *label)
{
    return 56 + Canvas::measure(font_label, label) + 44;
}

// Word-wrap `text` into lines no wider than `max_w`. Returns the height used.
int draw_wrapped(Canvas &c, const Font &f, int x, int top, int max_w, const char *text,
                 uint32_t color, int line_gap = 6, int alpha = 255)
{
    char line[256];
    int y = top;
    const char *p = text;
    while (*p)
    {
        int len = 0, last_space = -1;
        line[0] = 0;
        while (p[len] && p[len] != '\n' && len < (int)sizeof(line) - 1)
        {
            memcpy(line, p, (size_t)len + 1);
            line[len + 1] = 0;
            if (Canvas::measure(f, line) > max_w && last_space > 0)
                break;
            if (p[len] == ' ')
                last_space = len;
            ++len;
        }
        int take = len;
        if (p[len] && p[len] != '\n' && last_space > 0)
            take = last_space;
        memcpy(line, p, (size_t)take);
        line[take] = 0;
        c.text(f, x, y, line, color, alpha);
        y += f.height + line_gap;
        p += take;
        while (*p == ' ' || *p == '\n')
            ++p;
    }
    return y - top;
}

void draw_steps(Canvas &c, int x, int top, int w, const UiModel &m)
{
    static const char *kTitle[3] = {"Start the stream on your Vita", "Connect the USB cable",
                                    "That's it"};
    static const char *kBody[3] = {
        "Open VitaUSBStream and turn USB streaming on.",
        "Plug the Vita into a USB port on the front of the PS4.",
        "VITA4 finds the Vita and brings up its screen by itself."};
    const int row_h = 112, pad = 40;
    const int h = pad * 2 + row_h * 3 - 24;
    c.round_rect((float)x, (float)top, (float)w, (float)h, 26, kPanel, 222);
    c.round_rect_outline((float)x, (float)top, (float)w, (float)h, 26, 1.5f, kOutline);
    int active = m.phase == Phase::connecting ? 2 : -1;
    for (int i = 0; i < 3; ++i)
    {
        const int ry = top + pad + i * row_h;
        const float cx = (float)(x + pad + 28), cy = (float)(ry + 30);
        const uint32_t ring_color = i == active ? kSignal : 0x3c5a86;
        c.circle(cx, cy, 28, kPanelHigh);
        c.ring(cx, cy, 27, 2.5f, ring_color);
        char n[2] = {(char)('1' + i), 0};
        c.text_centered(font_label, (int)cx, (int)cy - font_label.height / 2, n,
                        i == active ? kSignal : kInk);
        c.text(font_label, x + pad + 80, ry + 2, kTitle[i], kInk);
        c.text(font_small, x + pad + 80, ry + 44, kBody[i], kMuted);
    }
}

void draw_log(Canvas &c, int x, int top, int w, const UiModel &m)
{
    const int line_h = font_small.height + 4;
    const int pad = 32;
    const int h = pad * 2 + 52 + line_h * VD4_LOG_LINES;
    c.round_rect((float)x, (float)top, (float)w, (float)h, 26, kPanel, 235);
    c.round_rect_outline((float)x, (float)top, (float)w, (float)h, 26, 1.5f, kOutline);
    c.text(font_label, x + pad, top + pad - 4, "Log", kInk);
    c.text_right(font_small, x + w - pad, top + pad, "Also saved to /data/vita4/vita4.log",
                 kMuted);
    for (int i = 0; i < m.log_count && i < VD4_LOG_LINES; ++i)
    {
        char clipped[VD4_TEXT];
        snprintf(clipped, sizeof(clipped), "%s", m.log[i]);
        // Trim to the panel width.
        size_t n = strlen(clipped);
        while (n > 4 && Canvas::measure(font_small, clipped) > w - pad * 2)
        {
            clipped[--n] = 0;
            clipped[n - 1] = clipped[n - 2] = clipped[n - 3] = '.';
        }
        c.text(font_small, x + pad, top + pad + 52 + i * line_h, clipped, 0xb9c8e2);
    }
}

void animated_dots(char *out, size_t cap, const char *base, double t)
{
    const int n = (int)(fmod(t, 1.6) / 0.4);
    snprintf(out, cap, "%s%.*s", base, n, "...");
}

} // namespace

void ui_init_model(UiModel &m)
{
    m = UiModel();
    m.status[0] = m.detail[0] = m.toast[0] = m.audio_note[0] = 0;
    for (auto &l : m.log)
        l[0] = 0;
}

void ui_draw_screen(Canvas &c, const UiModel &m, const uint32_t *backdrop)
{
    backdrop_or_fill(c, backdrop);

    draw_brand(c, kMargin, 64, font_heading);
    const char *pill;
    uint32_t pill_color;
    phase_pill(m, &pill, &pill_color);
    const float breathe = (float)(0.5 + 0.5 * sin(m.time * 3.0));
    draw_pill(c, c.width() - kMargin, 64, pill, pill_color, breathe);

    const char *title = "Plug in your PS Vita";
    const char *sub =
        "Play your Vita on the TV with your PS4 controller. The picture and sound come "
        "through the USB cable.";
    if (m.phase == Phase::starting)
        title = "Getting ready";
    else if (m.phase == Phase::connecting)
        title = "Vita found";
    else if (m.phase == Phase::usb_error)
    {
        title = "USB isn't available";
        sub = "VITA4 needs a jailbroken PS4 with GoldHEN running so it can talk to the Vita "
              "over USB.";
    }

    const int col_w = 880;
    const int card_top = 420;
    if (m.show_log)
    {
        draw_log(c, kMargin, 180, c.width() - kMargin * 2, m);
    }
    else
    {
        c.text(font_title, kMargin - 4, 200, title, kInk);
        draw_wrapped(c, font_body, kMargin, 292, col_w, sub, kMuted);
        draw_steps(c, kMargin, card_top, col_w, m);
    }

    if (!m.show_log)
    {
        // Live status line under the card.
        char line[VD4_TEXT + 8];
        if (m.phase == Phase::waiting || m.phase == Phase::starting ||
            m.phase == Phase::connecting)
            animated_dots(line, sizeof(line), m.status, m.time);
        else
            snprintf(line, sizeof(line), "%s", m.status);
        const int sy = card_top + 40 * 2 + 112 * 3 - 24 + 44; // below the steps card
        c.circle((float)(kMargin + 9), (float)(sy + font_body.height / 2), 6, pill_color);
        c.text(font_body, kMargin + 30, sy, line, kInk);
        if (m.detail[0])
            draw_wrapped(c, font_small, kMargin + 30, sy + font_body.height + 8, col_w - 30,
                         m.detail, kMuted);
    }

    if (m.prev_crash)
    {
        const char *note = m.show_log
                               ? "The last lines VITA4 wrote before it stopped last time:"
                               : "VITA4 stopped while upscaling last time, so Upscale is off. "
                                 "Press triangle to see why.";
        c.text(font_small, kMargin, 140, note, kWaiting);
    }

    // Footer hints.
    const int fy = c.height() - 96;
    int hx = kMargin;
    const char *log_label = m.show_log ? "Hide log" : "Show log";
    draw_hint(c, hx, fy, FaceButton::triangle, log_label);
    hx += hint_width(log_label);
    if (m.phase == Phase::waiting || m.phase == Phase::usb_error)
    {
        draw_hint(c, hx, fy, FaceButton::cross, "Try again");
        hx += hint_width("Try again");
    }
    draw_hint(c, hx, fy, FaceButton::circle, "Exit");
}

void ui_draw_no_signal(Canvas &c, const UiModel &m, const uint32_t *backdrop)
{
    (void)backdrop; // the emblem would sit behind the card: plain gradient here
    c.vgradient(0, 0, c.width(), c.height(), 0x050a14, 0x0b1b33);
    draw_brand(c, kMargin, 64, font_heading);
    const char *pill;
    uint32_t pill_color;
    phase_pill(m, &pill, &pill_color);
    draw_pill(c, c.width() - kMargin, 64, pill, pill_color,
              (float)(0.5 + 0.5 * sin(m.time * 3.0)));

    const int w = 980, h = 300;
    const int x = (c.width() - w) / 2, y = (c.height() - h) / 2 + 40;
    c.round_rect((float)x, (float)y, (float)w, (float)h, 28, kPanel, 230);
    c.round_rect_outline((float)x, (float)y, (float)w, (float)h, 28, 1.5f, kOutline);
    // Scanning sweep across the top edge of the card.
    const float sweep = (float)fmod(m.time * 0.5, 1.0);
    c.round_rect((float)(x + 40), (float)(y + 36), (float)(w - 80), 6, 3, kPanelHigh);
    c.round_rect((float)(x + 40 + (w - 80 - 160) * sweep), (float)(y + 36), 160, 6, 3, kSignal);

    c.text_centered(font_heading, c.width() / 2, y + 78, "Connected - waiting for the picture",
                    kInk);
    const char *body = m.since_signal > 6
                           ? "Wake the Vita's screen. If the picture doesn't come back, unplug "
                             "the cable and plug it in again."
                           : "The Vita is connected. Its screen will appear here in a moment.";
    draw_wrapped(c, font_body, x + 70, y + 160, w - 140, body, kMuted);

    // Menu hint.
    const char *hint = "Press L3 + R3 together for the menu";
    c.text_centered(font_small, c.width() / 2, c.height() - 96, hint, kMuted);
}

bool ui_live_needs_overlay(const UiModel &m)
{
    return m.menu_open || m.settings.stats || m.time < m.hint_until || m.time < m.toast_until;
}

const char *ui_input_test_name(int test)
{
    static const char *k[] = {"Home screen swipes", "Button tour", "Front tap + swipe",
                              "Rear swipe", "Release all"};
    return k[(test % VD4_INPUT_TESTS + VD4_INPUT_TESTS) % VD4_INPUT_TESTS];
}

const char *ui_menu_value(const UiModel &m, int item)
{
    switch (item)
    {
        case MENU_PASSTHROUGH:
            return m.settings.passthrough ? "On" : "Off";
        case MENU_TOUCH:
        {
            static const char *k[] = {"Rear touchpad", "Front screen", "Off"};
            return k[m.settings.touch_target % 3];
        }
        case MENU_TAPS:
        {
            static const char *k[] = {"Steady", "Slow games", "Raw"};
            return k[(m.settings.taps % 3 + 3) % 3];
        }
        case MENU_INPUT_TEST:
            return m.input_test_running ? "Running..." : ui_input_test_name(m.input_test);
        case MENU_PICTURE:
        {
            if (m.settings.upscale == VD_UPSCALE_TV)
                return "Set by the TV scaler";
            if (m.settings.upscale != VD_UPSCALE_OFF)
                return "2x (set by Upscale)";
            static const char *k[] = {"Sharp (2x)", "Fit to screen", "Original size"};
            return k[m.settings.scale_mode % 3];
        }
        case MENU_PICSIZE:
            return m.settings.fast_picture ? "Fast 480x272 (less lag)" : "Full 960x544";
        case MENU_UPSCALE:
            if (vd_upscale_uses_worker(m.settings.upscale))
                return m.settings.upscale == VD_UPSCALE_FSR ? "FSR (slow)" : "Sharp (slow)";
            if (m.settings.upscale == VD_UPSCALE_TV)
                return m.tv_scaler < 0 ? "TV scaler (not supported)" : "TV scaler (test)";
            return vd_upscale_label(m.settings.upscale);
        case MENU_SHARPNESS:
        {
            static const char *k[] = {"Off", "Low", "Medium", "High", "Max"};
            return k[(m.settings.sharpness % 5 + 5) % 5];
        }
        case MENU_SOUND:
            return m.audio_state == 2 ? "Not available" : (m.settings.sound ? "On" : "Off");
        case MENU_DEADZONE:
            return m.settings.deadzone ? "On" : "Off";
        case MENU_LOW_LAG:
            return m.settings.low_lag ? "On" : "Off";
        case MENU_STATS:
            return m.settings.stats ? "On" : "Off";
        default:
            return nullptr;
    }
}

void ui_draw_live_overlay(Canvas &c, const UiModel &m)
{
    // Stats corner.
    if (m.settings.stats && !m.menu_open)
    {
        char l1[96], l2[96], l3[96], l4[96];
        snprintf(l1, sizeof(l1), "%dx%d   %.0f fps in  /  %.0f fps out", m.video_w, m.video_h,
                 m.capture_fps, m.present_fps);
        snprintf(l2, sizeof(l2), "Frames %u   dropped %u   late %u", m.frames, m.bad_frames,
                 m.late_flips);
        char snd[32];
        if (m.audio_state == 1)
            snprintf(snd, sizeof(snd), "on (%.0f ms)", m.audio_buffer_ms);
        else
            snprintf(snd, sizeof(snd), "%s", m.audio_state == 2 ? "n/a" : "off");
        if (!m.settings.passthrough)
            snprintf(l3, sizeof(l3), "Sound %s   Input: off", snd);
        else
            snprintf(l3, sizeof(l3), "Sound %s   Input sent %llu%s", snd,
                     (unsigned long long)m.input_sent,
                     m.pad_link ? "" : (m.controller ? " (USB error)" : " (no pad)"));
        l4[0] = 0;
        if (m.settings.upscale != VD_UPSCALE_OFF)
        {
            if (m.windowed)
                snprintf(l4, sizeof(l4), "Upscale %s   (full screen only)",
                         vd_upscale_label(m.settings.upscale));
            else if (m.settings.upscale == VD_UPSCALE_SMOOTH)
                snprintf(l4, sizeof(l4), "Upscale Smooth   every frame");
            else if (m.settings.upscale == VD_UPSCALE_TV)
                snprintf(l4, sizeof(l4), "Upscale TV scaler   %s",
                         m.tv_scaler > 0 ? "PS4 display hardware"
                                         : (m.tv_scaler < 0 ? "refused" : "starting"));
            else if (m.upscale_active)
                snprintf(l4, sizeof(l4), "Upscale %s   %.0f fps   %.0f ms",
                         vd_upscale_label(m.settings.upscale), m.upscale_fps, m.upscale_ms);
            else
                snprintf(l4, sizeof(l4), "Upscale %s   starting",
                         vd_upscale_label(m.settings.upscale));
        }
        // Lag VITA4 can measure (the game's own and the TV's are not visible).
        char l5[128], b[24], d[24];
        auto ms = [](char *o, size_t n, float v) {
            if (v > 0)
                snprintf(o, n, "%.0f ms", v);
            else
                snprintf(o, n, "--");
        };
        ms(b, sizeof(b), m.lag_transfer_ms);
        ms(d, sizeof(d), m.lag_screen_ms);
        snprintf(l5, sizeof(l5), "Lag  USB picture %s   to screen %s", b, d);
        char l6[128];
        if (!m.settings.low_lag)
            snprintf(l6, sizeof(l6), "Low lag Off");
        else if (m.latch_state == 2)
            snprintf(l6, sizeof(l6), "Low lag On   draw at %.1f ms   arrivals %.1f ms wide",
                     m.latch_ms, m.latch_spread_ms);
        else if (m.latch_state == 3)
            snprintf(l6, sizeof(l6), "Low lag On   irregular (%.1f ms): Off timing",
                     m.latch_spread_ms);
        else if (m.latch_state == 1)
            snprintf(l6, sizeof(l6), "Low lag On   measuring...");
        else
            l6[0] = 0;
        // 1.38: what the controller's sticks read (0 = full left/up,
        // 255 = full right/down, 128 = centre) - the Vita gets the same.
        char l7[96];
        snprintf(l7, sizeof(l7), "Sticks   left %3d %3d   right %3d %3d", m.stick_lx, m.stick_ly,
                 m.stick_rx, m.stick_ry);
        const char *lines[7] = {l1, l2, l3, l4, l5, l6, l7};
        int n = 0;
        const char *show[7];
        for (int i = 0; i < 7; ++i)
            if (lines[i][0])
                show[n++] = lines[i];
        const int w = 660, h = 32 + 36 * n, x = c.width() - w - 40, y = 40;
        c.round_rect((float)x, (float)y, (float)w, (float)h, 18, 0x04070f, 190);
        for (int i = 0; i < n; ++i)
            c.text(font_small, x + 24, y + 20 + 36 * i, show[i], i == 0 ? kInk : kMuted);
    }

    // Toast (top centre).
    if (m.time < m.toast_until && m.toast[0])
    {
        const int a = fade_alpha(m.time, m.toast_until);
        const int tw = Canvas::measure(font_label, m.toast);
        const int w = tw + 72, h = 64, x = (c.width() - w) / 2, y = 48;
        c.round_rect((float)x, (float)y, (float)w, (float)h, 32, kPanel, a * 235 / 255);
        c.text(font_label, x + 36, y + (h - font_label.height) / 2, m.toast, kInk, a);
    }

    // First-run hint (bottom centre).
    if (m.time < m.hint_until && !m.menu_open)
    {
        const int a = fade_alpha(m.time, m.hint_until, 0.8);
        const char *t1 = "Press ";
        const char *t2 = "L3 + R3";
        const char *t3 = " together for the menu";
        const int w1 = Canvas::measure(font_label, t1), w2 = Canvas::measure(font_label, t2),
                  w3 = Canvas::measure(font_label, t3);
        const int w = w1 + w2 + w3 + 32 + 72, h = 68;
        const int x = (c.width() - w) / 2, y = c.height() - h - 64;
        c.round_rect((float)x, (float)y, (float)w, (float)h, 34, kPanel, a * 230 / 255);
        int pen = x + 36;
        const int ty = y + (h - font_label.height) / 2;
        pen += c.text(font_label, pen, ty, t1, kInk, a);
        c.round_rect((float)(pen + 4), (float)(y + 12), (float)(w2 + 24), (float)(h - 24), 10,
                     kSignal, a * 40 / 255);
        pen += 16;
        pen += c.text(font_label, pen, ty, t2, kSignal, a);
        pen += 16;
        c.text(font_label, pen, ty, t3, kInk, a);
    }

    if (!m.menu_open)
        return;

    // Menu panel.
    c.blend_rect(0, 0, c.width(), c.height(), 0x02040a, 120);
    static const char *kLabels[MENU_COUNT] = {
        "Back to the game", "Controller to Vita",   "Touchpad",   "Touch taps", "Picture size",
        "Picture resolution",
        "Upscale",          "Sharpness",            "Sound",      "Stick dead zone", "Low lag",
        "Stats",
        "Press Vita PS button",
        "Input test",       "Restart stream",       "Exit VITA4"};
    const int row_h = 50, pad = 40;
    const int w = 820, h = pad * 2 + 90 + row_h * MENU_COUNT + 70;
    const int x = 140, y = (c.height() - h) / 2;
    c.round_rect((float)x, (float)y, (float)w, (float)h, 30, kPanel, 245);
    c.round_rect_outline((float)x, (float)y, (float)w, (float)h, 30, 1.5f, kOutline);
    draw_brand(c, x + pad, y + pad - 6, font_heading);

    for (int i = 0; i < MENU_COUNT; ++i)
    {
        const int ry = y + pad + 90 + i * row_h;
        const bool sel = i == m.menu_sel;
        if (sel)
        {
            c.round_rect((float)(x + 20), (float)ry, (float)(w - 40), (float)(row_h - 8), 16,
                         kPanelHigh);
            c.round_rect((float)(x + 20), (float)(ry + 14), 6, (float)(row_h - 36), 3, kSignal);
        }
        const int ty = ry + (row_h - 8 - font_label.height) / 2;
        const uint32_t label_color = i == MENU_EXIT ? 0xff8f9a : kInk;
        c.text(sel ? font_label : font_body, x + pad + 12, ty, kLabels[i], label_color);
        const char *val = ui_menu_value(m, i);
        if (val)
        {
            const int vx = x + w - pad - 8;
            if (sel)
            {
                const int vw = Canvas::measure(font_body, val);
                c.text(font_body, vx - 30 - vw - 34, ty, "<", kSignal);
                c.text_right(font_body, vx, ty, ">", kSignal);
                c.text_right(font_body, vx - 30, ty, val, kInk);
            }
            else
            {
                c.text_right(font_body, vx, ty, val, kMuted);
            }
        }
    }

    const int fy = y + h - pad - 44;
    int hx = x + pad;
    draw_hint(c, hx, fy, FaceButton::cross, "Select");
    hx += hint_width("Select");
    draw_hint(c, hx, fy, FaceButton::circle, "Back");

    // Status line on the right of the panel.
    char st[128];
    if (m.input_test_running && m.input_test_step[0])
        snprintf(st, sizeof(st), "%s", m.input_test_step);
    else
        snprintf(st, sizeof(st), "%s", m.video_signal ? "Picture: live" : "Picture: waiting");
    c.text_right(font_small, x + w - pad, fy + 10, st, kMuted);

    // Controller shortcuts (VITA5's L3 chords), to the right of the menu.
    static const char *kKeys[][2] = {{"L3 + R3", "Open / close this menu"},
                                     {"L3 + Left / Right", "Upscale: Off, Smooth, TV, ..."},
                                     {"L3 + Up", "Full screen / windowed"}};
    const int cx = x + w + 60, cw = 700, ch = 70 + 3 * 74 + 20;
    const int cy = y + h - ch;
    c.round_rect((float)cx, (float)cy, (float)cw, (float)ch, 26, kPanel, 235);
    c.round_rect_outline((float)cx, (float)cy, (float)cw, (float)ch, 26, 1.5f, kOutline);
    c.text(font_label, cx + 36, cy + 26, "Shortcuts", kInk);
    for (int i = 0; i < 3; ++i)
    {
        const int ry = cy + 84 + i * 74;
        const int kw = Canvas::measure(font_body, kKeys[i][0]);
        c.round_rect((float)(cx + 30), (float)ry, (float)(kw + 32), 50, 12, kSignal, 36);
        c.text(font_body, cx + 46, ry + (50 - font_body.height) / 2, kKeys[i][0], kSignal);
        c.text(font_body, cx + 30 + kw + 32 + 22, ry + (50 - font_body.height) / 2, kKeys[i][1],
               kMuted);
    }
}

void ui_window_rect(int *x, int *y, int *w, int *h)
{
    // 1.5x the Vita's 960x544, under the header.
    *w = 1440;
    *h = 816;
    *x = (1920 - *w) / 2;
    *y = 150;
}

void ui_draw_window_frame(Canvas &c, const UiModel &m)
{
    c.vgradient(0, 0, c.width(), c.height(), 0x050a14, 0x0b1b33);
    draw_brand(c, kMargin, 52, font_heading);
    const char *pill;
    uint32_t pill_color;
    phase_pill(m, &pill, &pill_color);
    draw_pill(c, c.width() - kMargin, 52, pill, pill_color,
              (float)(0.5 + 0.5 * sin(m.time * 3.0)));
    int x, y, w, h;
    ui_window_rect(&x, &y, &w, &h);
    c.round_rect((float)(x - 10), (float)(y - 10), (float)(w + 20), (float)(h + 20), 22, kPanel);
    c.round_rect_outline((float)(x - 10), (float)(y - 10), (float)(w + 20), (float)(h + 20), 22,
                         1.5f, kOutline);
    char foot[160];
    snprintf(foot, sizeof(foot), "L3 + Up: full screen      L3 + R3: menu      %s",
             m.video_signal ? "" : "Waiting for the picture");
    c.text_centered(font_small, c.width() / 2, y + h + 34, foot, kMuted);
}

} // namespace vd4
