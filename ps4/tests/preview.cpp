// VITA4 - host-side screen previews: renders every UI state to PNG files so
// the PS4 screens can be reviewed without a console.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "canvas.h"
#include "nv12_scale.h"
#include "upscale.h"
#include "ui.h"

using namespace vd4;

static const int W = 1920, H = 1080;

static void save(const char *dir, const char *name, const std::vector<uint32_t> &px)
{
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i)
    {
        rgb[i * 3 + 0] = (uint8_t)(px[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(px[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)(px[i]);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    stbi_write_png(path, W, H, 3, rgb.data(), W * 3);
    printf("wrote %s\n", path);
}

// Synthetic Vita frame: colour bars + gradient, as NV12.
static std::vector<uint8_t> make_frame(int w, int h)
{
    std::vector<uint8_t> f((size_t)w * h * 3 / 2);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            f[(size_t)y * w + x] = (uint8_t)(16 + (219 * x) / w);
    uint8_t *uv = f.data() + (size_t)w * h;
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; x += 2)
        {
            uv[(size_t)y * w + x] = (uint8_t)(64 + (128 * y) / (h / 2));
            uv[(size_t)y * w + x + 1] = (uint8_t)(200 - (128 * x) / w);
        }
    return f;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *backdrop_path = argc > 2 ? argv[2] : "assets/backdrop.png";
    int bw = 0, bh = 0, bn = 0;
    unsigned char *img = stbi_load(backdrop_path, &bw, &bh, &bn, 4);
    std::vector<uint32_t> backdrop((size_t)W * H, 0xFF000000u);
    if (img && bw == W && bh == H)
    {
        for (size_t i = 0; i < (size_t)W * H; ++i)
            backdrop[i] = 0xFF000000u | ((uint32_t)img[i * 4] << 16) |
                          ((uint32_t)img[i * 4 + 1] << 8) | img[i * 4 + 2];
    }
    else
    {
        fprintf(stderr, "backdrop not loaded (%s)\n", backdrop_path);
    }

    std::vector<uint32_t> px((size_t)W * H);
    Canvas c(px.data(), W, H, W);
    UiModel m;
    ui_init_model(m);
    m.time = 1.0;

    m.phase = Phase::waiting;
    snprintf(m.status, sizeof(m.status), "Looking for your Vita");
    snprintf(m.detail, sizeof(m.detail), "USB is ready. Nothing plugged in yet.");
    ui_draw_screen(c, m, backdrop.data());
    save(dir, "1_waiting", px);

    m.phase = Phase::connecting;
    snprintf(m.status, sizeof(m.status), "Starting the picture");
    snprintf(m.detail, sizeof(m.detail), "Vita USB Stream found (054c:1338), 960x544 NV12.");
    ui_draw_screen(c, m, backdrop.data());
    save(dir, "2_connecting", px);

    m.phase = Phase::usb_error;
    snprintf(m.status, sizeof(m.status), "The PS4 USB library didn't start");
    snprintf(m.detail, sizeof(m.detail),
             "sceUsbdInit returned 0x80020001. Make sure GoldHEN is running, then try again.");
    ui_draw_screen(c, m, backdrop.data());
    save(dir, "3_usb_error", px);

    m.phase = Phase::waiting;
    m.show_log = true;
    for (int i = 0; i < 12; ++i)
        snprintf(m.log[m.log_count++], VD4_TEXT, "[%6.2f] example log line %d: USB scan found 3 devices", i * 0.5, i);
    ui_draw_screen(c, m, backdrop.data());
    save(dir, "4_log", px);
    m.show_log = false;

    m.phase = Phase::live;
    m.video_signal = false;
    m.since_signal = 2;
    ui_draw_no_signal(c, m, backdrop.data());
    save(dir, "5_no_signal", px);

    // Live with video + hint + toast.
    std::vector<uint8_t> frame = make_frame(960, 544);
    static VdScaleMap map;
    vd_scale_map_build(&map, 960, 544, W, H, VD_SCALE_SHARP);
    std::vector<uint32_t> scratch(960);
    VdUpscaler *up = vd_upscaler_create(960, 544);
    vd_upscale_frame(up, frame.data(), VD_UPSCALE_FSR, px.data(), W, 4, H);
    m.settings.upscale = VD_UPSCALE_FSR;
    m.upscale_active = true;
    m.upscale_fps = 24;
    m.upscale_ms = 38;
    m.video_signal = true;
    m.video_w = 960;
    m.video_h = 544;
    m.capture_fps = 58;
    m.lag_transfer_ms = 23;
    m.lag_screen_ms = 9;
    m.late_flips = 2;
    m.latch_state = 2;
    m.stick_rx = 255;
    m.stick_ry = 131;
    m.audio_buffer_ms = 14;
    m.latch_ms = 6.4f;
    m.latch_spread_ms = 2.1f;
    m.present_fps = 60;
    m.frames = 1234;
    m.audio_state = 1;
    m.pad_link = true;
    m.controller = true;
    m.hint_until = 5;
    snprintf(m.toast, sizeof(m.toast), "Upscale: FSR");
    m.toast_until = 3;
    m.settings.stats = true;
    ui_draw_live_overlay(c, m);
    save(dir, "6_live_overlays", px);

    vd_nv12_render_rows(frame.data(), &map, px.data(), W, 0, H, scratch.data());
    m.menu_open = true;
    m.menu_sel = MENU_UPSCALE;
    ui_draw_live_overlay(c, m);
    save(dir, "7_menu", px);

    vd_scale_map_build(&map, 960, 544, W, H, VD_SCALE_FIT);
    vd_nv12_render_rows(frame.data(), &map, px.data(), W, 0, H, scratch.data());
    save(dir, "8_fit", px);

    // Windowed (L3 + Up).
    m.menu_open = false;
    m.settings.stats = false;
    m.hint_until = 0;
    m.windowed = true;
    snprintf(m.toast, sizeof(m.toast), "Windowed");
    ui_draw_window_frame(c, m);
    int wx, wy, ww, wh;
    ui_window_rect(&wx, &wy, &ww, &wh);
    vd_scale_map_build(&map, 960, 544, ww, wh, VD_SCALE_FIT);
    vd_nv12_render_rows(frame.data(), &map, px.data() + (size_t)wy * W + wx, W, 0, wh,
                        scratch.data());
    ui_draw_live_overlay(c, m);
    save(dir, "9_windowed", px);
    vd_upscaler_destroy(up);
    return 0;
}
