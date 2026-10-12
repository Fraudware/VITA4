// VITA4 - host unit tests for the portable parts of the PS4 build.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The descriptor under test is rebuilt from the values confirmed on real
// hardware (docs/PS5_PROBE_FINDINGS.md, docs/STREAMING.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include "nv12_scale.h"
#include "pad_map.h"
#include "pacing.h"
#include "pad_wire.h"
#include "upscale.h"
#include "smooth.h"
#include "uvc_protocol.h"
#include "vita_desc.h"
#include "xfer_layout.h"
}
#include "audio_ring.h"
#include "canvas.h"
#include "display.h"
#include "settings_io.h"
#include "upscale_worker.h"
#include <math.h>
#include <chrono>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (cond)                                                                                  \
            ++g_pass;                                                                              \
        else                                                                                       \
        {                                                                                          \
            ++g_fail;                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

// ---- synthetic Vita configuration descriptor ---------------------------------

struct Desc
{
    std::vector<uint8_t> b;
    void add(std::initializer_list<int> v)
    {
        for (int x : v)
            b.push_back((uint8_t)x);
    }
    void u16(unsigned v)
    {
        b.push_back(v & 0xff);
        b.push_back((v >> 8) & 0xff);
    }
    void u32(unsigned v)
    {
        u16(v & 0xffff);
        u16(v >> 16);
    }
};

static std::vector<uint8_t> vita_config(bool with_pad_iface)
{
    Desc d;
    d.add({9, 2, 0, 0, with_pad_iface ? 5 : 4, 1, 0, 0x80, 250}); // config (length patched)
    d.add({8, 0x0B, 0, 2, 0x0E, 0x03, 0, 0});                       // IAD video
    d.add({9, 4, 0, 0, 0, 0x0E, 0x01, 0, 0});                       // VC iface 0
    d.add({13, 0x24, 0x01});                                        // VC header
    d.u16(0x0110);
    d.u16(13);
    d.u32(48000000);
    d.add({1, 1});
    d.add({9, 4, 1, 0, 1, 0x0E, 0x02, 0, 0}); // VS iface 1 alt 0, 1 endpoint
    // VS input header: bLength 14, numFormats 1, wTotalLength, ep 0x81 ...
    d.add({14, 0x24, 0x01, 1});
    d.u16(0); // wTotalLength (unused by the parser)
    d.add({0x81, 0, 0, 0, 0, 0, 1, 0});
    // Uncompressed format: 27 bytes, NV12 GUID, 12 bpp, 5 frames.
    d.add({27, 0x24, 0x04, 1, 5});
    d.add({'N', 'V', '1', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b,
           0x71});
    d.add({12, 1, 0, 0, 0, 0});
    const int sizes[5][2] = {{960, 544}, {896, 504}, {864, 488}, {480, 272}, {640, 480}};
    for (int i = 0; i < 5; ++i)
    {
        d.add({34, 0x24, 0x05, i + 1, 0});
        d.u16(sizes[i][0]);
        d.u16(sizes[i][1]);
        d.u32(1000000);
        d.u32(1000000);
        d.u32(sizes[i][0] * sizes[i][1] * 3 / 2);
        d.u32(166666);
        d.add({2});
        d.u32(166666);
        d.u32(333333);
    }
    d.add({7, 5, 0x81, 0x02});
    d.u16(512);
    d.add({0});
    d.add({8, 0x0B, 2, 2, 0x01, 0x00, 0, 0}); // IAD audio
    d.add({9, 4, 2, 0, 0, 0x01, 0x01, 0, 0}); // AC iface 2
    d.add({9, 0x24, 0x01, 0x00, 0x01, 30, 0, 1, 3});
    d.add({9, 4, 3, 0, 0, 0x01, 0x02, 0, 0}); // AS iface 3 alt 0
    d.add({9, 4, 3, 1, 1, 0x01, 0x02, 0, 0}); // AS iface 3 alt 1
    d.add({7, 0x24, 0x01, 3, 1, 1, 0});
    d.add({11, 0x24, 0x02, 1, 2, 2, 16, 1, 0x80, 0xBB, 0x00});
    d.add({9, 5, 0x82, 0x05});
    d.u16(192);
    d.add({4, 0, 0});
    d.add({7, 0x25, 0x01, 0, 0, 0, 0});
    if (with_pad_iface)
    {
        d.add({9, 4, 4, 0, 1, 0xFF, 0x50, 0x01, 0});
        d.add({7, 5, 0x03, 0x02});
        d.u16(512);
        d.add({0});
    }
    d.b[2] = d.b.size() & 0xff;
    d.b[3] = (d.b.size() >> 8) & 0xff;
    return d.b;
}

static void test_descriptor()
{
    for (int pad = 0; pad < 2; ++pad)
    {
        std::vector<uint8_t> cfg = vita_config(pad != 0);
        VdUsbVitaDevice dev;
        memset(&dev, 0, sizeof(dev));
        dev.vid = 0x054C;
        dev.pid = 0x1338;
        CHECK(vd_parse_vita_config(cfg.data(), (unsigned)cfg.size(), &dev) == 0);
        CHECK(dev.is_vita == 1);
        CHECK(dev.vid == 0x054C && dev.pid == 0x1338);
        CHECK(dev.uvc.interface_number == 1);
        CHECK(dev.video_in_count == 1 && dev.video_in_eps[0] == 0x81);
        CHECK(dev.has_video_control && dev.video_control_interface == 0);
        CHECK(dev.uvc.format_count == 1);
        CHECK(dev.uvc.formats[0].is_nv12 == 1);
        CHECK(dev.uvc.formats[0].frame_count == 5);
        CHECK(dev.uvc.formats[0].frames[0].width == 960);
        CHECK(dev.uvc.formats[0].frames[0].height == 544);
        CHECK(dev.uvc.formats[0].frames[0].default_interval == 166666);
        CHECK(dev.audio_isoc_ep == 0x82);
        CHECK(dev.audio_isoc_maxpkt == 192);
        CHECK(dev.audio_isoc_alt == 1);
        CHECK(dev.audio_stream_interface == 3);
        CHECK(dev.has_pad == pad);
        if (pad)
            CHECK(dev.pad_out_ep == 0x03 && dev.pad_interface == 4);

        uint8_t fmt = 0;
        const VdUvcFrame *fr = nullptr;
        CHECK(vd_pick_default_frame(&dev, &fmt, &fr) == 0);
        CHECK(fmt == 1 && fr && fr->frame_index == 1);
        CHECK(vd_pick_frame(&dev, 480, 272, &fmt, &fr) == 0 && fr && fr->frame_index == 4 &&
              fr->width == 480 && fr->height == 272);
        CHECK(vd_pick_frame(&dev, 123, 45, &fmt, &fr) == 0 && fr && fr->frame_index == 1);
        CHECK(vd_largest_nv12_frame(&dev) == 783360);
        uint16_t w = 0, h = 0;
        CHECK(vd_match_nv12_frame(&dev, 783360, 1, &w, &h) && w == 960 && h == 544);
        CHECK(vd_match_nv12_frame(&dev, 195840, 1, &w, &h) && w == 480 && h == 272);
        CHECK(!vd_match_nv12_frame(&dev, 783361, 1, &w, &h));
    }
    // Garbage and truncation never crash and never match.
    std::vector<uint8_t> cfg = vita_config(false);
    for (size_t cut = 0; cut < cfg.size(); cut += 7)
    {
        VdUsbVitaDevice dev;
        memset(&dev, 0, sizeof(dev));
        vd_parse_vita_config(cfg.data(), (unsigned)cut, &dev);
    }
    srand(7);
    for (int iter = 0; iter < 2000; ++iter)
    {
        std::vector<uint8_t> junk = cfg;
        for (int k = 0; k < 6; ++k)
            junk[(size_t)rand() % junk.size()] = (uint8_t)rand();
        VdUsbVitaDevice dev;
        memset(&dev, 0, sizeof(dev));
        vd_parse_vita_config(junk.data(), (unsigned)junk.size(), &dev);
    }
    VdUsbVitaDevice none;
    memset(&none, 0, sizeof(none));
    uint16_t w, h;
    CHECK(vd_match_nv12_frame(&none, 783360, 1, &w, &h) && w == 960); // fallback
    CHECK(sizeof(VdUvcStreamingControl) == 34);
}

// ---- NV12 scaling ------------------------------------------------------------

static std::vector<uint8_t> noise_frame(int w, int h, unsigned seed)
{
    std::vector<uint8_t> f((size_t)w * h * 3 / 2);
    for (auto &b : f)
    {
        seed = seed * 1103515245u + 12345u;
        b = (uint8_t)(seed >> 16);
    }
    return f;
}

static void test_scaler()
{
    vd_yuv_tables_init();
    const int W = 1920, H = 1080;
    static VdScaleMap map;
    std::vector<uint32_t> out((size_t)W * H);
    std::vector<uint32_t> scratch(2048);

    // Sharp 2x for the Vita's 960x544: crops 4 output lines top and bottom.
    std::vector<uint8_t> f = noise_frame(960, 544, 1);
    CHECK(vd_scale_map_build(&map, 960, 544, W, H, VD_SCALE_SHARP) == 0);
    CHECK(map.xmap[0] == 0 && map.xmap[1] == 0 && map.xmap[2] == 1 && map.xmap[1919] == 959);
    CHECK(map.ymap[0] == 2 && map.ymap[1079] == 541);
    vd_nv12_render_rows(f.data(), &map, out.data(), W, 0, H, scratch.data());
    int mismatches = 0;
    for (int y = 0; y < H; y += 7)
        for (int x = 0; x < W; x += 5)
        {
            const uint32_t ref = vd_nv12_pixel_ref(f.data(), 960, 544, map.xmap[x], map.ymap[y]);
            if (out[(size_t)y * W + x] != ref)
                ++mismatches;
        }
    CHECK(mismatches == 0);

    // Fit: whole picture visible, centred, black side bars.
    CHECK(vd_scale_map_build(&map, 960, 544, W, H, VD_SCALE_FIT) == 0);
    CHECK(map.ymap[0] == 0 && map.ymap[1079] == 543);
    CHECK(map.xmap[0] == -1 && map.xmap[1919] == -1);
    vd_nv12_render_rows(f.data(), &map, out.data(), W, 0, H, scratch.data());
    CHECK(out[0] == VD_BLACK && out[1919] == VD_BLACK);
    CHECK(out[(size_t)540 * W + 960] == vd_nv12_pixel_ref(f.data(), 960, 544, map.xmap[960],
                                                          map.ymap[540]));

    // Original size: centred 1:1.
    CHECK(vd_scale_map_build(&map, 960, 544, W, H, VD_SCALE_ORIGINAL) == 0);
    CHECK(map.xmap[480] == 0 && map.xmap[479] == -1 && map.xmap[1439] == 959 &&
          map.xmap[1440] == -1);
    CHECK(map.ymap[268] == 0 && map.ymap[267] == -1);

    // Other Vita modes never index outside the frame, in every mode.
    const int modes[5][2] = {{960, 544}, {896, 504}, {864, 488}, {480, 272}, {640, 480}};
    for (auto &m : modes)
        for (int mode = 0; mode < VD_SCALE_COUNT; ++mode)
        {
            CHECK(vd_scale_map_build(&map, m[0], m[1], W, H, mode) == 0);
            bool ok = true;
            for (int x = 0; x < W; ++x)
                ok &= map.xmap[x] >= -1 && map.xmap[x] < m[0];
            for (int y = 0; y < H; ++y)
                ok &= map.ymap[y] >= -1 && map.ymap[y] < m[1];
            CHECK(ok);
            std::vector<uint8_t> fr = noise_frame(m[0], m[1], 3);
            vd_nv12_render_rows(fr.data(), &map, out.data(), W, 0, H, scratch.data());
        }

    // Known colours: limited-range black / white / mid grey.
    std::vector<uint8_t> flat(960 * 544 * 3 / 2, 128);
    memset(flat.data(), 16, 960 * 544);
    CHECK((vd_nv12_pixel_ref(flat.data(), 960, 544, 5, 5) & 0xFFFFFF) == 0x000000);
    memset(flat.data(), 235, 960 * 544);
    CHECK((vd_nv12_pixel_ref(flat.data(), 960, 544, 5, 5) & 0xFFFFFF) == 0xFFFFFF);

    // Banded rendering (as the PS4 row pool does) equals one pass.
    vd_scale_map_build(&map, 960, 544, W, H, VD_SCALE_FIT);
    std::vector<uint32_t> a((size_t)W * H), b((size_t)W * H);
    vd_nv12_render_rows(f.data(), &map, a.data(), W, 0, H, scratch.data());
    vd4::RowPool pool(3);
    struct Ctx
    {
        const uint8_t *f;
        VdScaleMap *map;
        uint32_t *dst;
        std::vector<uint32_t> s[4];
    } ctx{f.data(), &map, b.data(), {}};
    for (auto &s : ctx.s)
        s.resize(2048);
    pool.run(
        [](void *c, int b0, int b1, int wk) {
            Ctx *x = (Ctx *)c;
            vd_nv12_render_rows(x->f, x->map, x->dst, 1920, b0, b1, x->s[wk].data());
        },
        &ctx, H);
    CHECK(a == b);
    for (int rep = 0; rep < 50; ++rep)
        pool.run([](void *, int, int, int) {}, nullptr, H);
}

// ---- controller mapping --------------------------------------------------------

static void test_pad()
{
    // DS4 bit values match the Vita's SceCtrl layout except OPTIONS/touch.
    CHECK(vd_ds4_buttons_to_vita(DS4_CROSS) == VD_PAD_CROSS);
    CHECK(vd_ds4_buttons_to_vita(DS4_OPTIONS) == VD_PAD_START);
    CHECK(vd_ds4_buttons_to_vita(DS4_TOUCH_PAD) == VD_PAD_SELECT);
    CHECK(vd_ds4_buttons_to_vita(DS4_UP | DS4_SQUARE | DS4_R1) ==
          (VD_PAD_UP | VD_PAD_SQUARE | VD_PAD_R1));
    CHECK(vd_ds4_buttons_to_vita(DS4_CROSS | DS4_INTERCEPTED) == 0);
    CHECK(vd_axis_u8_to_i16(0) == -32768);
    CHECK(vd_axis_u8_to_i16(128) == 0);
    CHECK(vd_axis_u8_to_i16(255) == 32639); // same as the PS5 build (pad_bridge.cpp)
    // Round trip through the Vita-side conversion: centre stays centre.
    CHECK(pad_wire_stick_to_u8(vd_axis_u8_to_i16(128)) == 0x80);
    CHECK(pad_wire_stick_to_u8(vd_axis_u8_to_i16(0)) == 0x00);
    CHECK(pad_wire_stick_to_u8(vd_axis_u8_to_i16(255)) == 0xFF);

    Ds4State s;
    memset(&s, 0, sizeof(s));
    s.connected = 1;
    s.buttons = DS4_CROSS | DS4_L2;
    s.lx = 0;
    s.ly = 255;
    s.rx = s.ry = 128;
    s.l2 = 200;
    VdPadReport r;
    vd_ds4_make_report(&s, 7, 123456, &r);
    uint8_t wire[VD_PAD_WIRE_BYTES];
    CHECK(vd_pad_serialize(&r, wire, sizeof(wire)) == 28);
    pad_wire_report dec;
    CHECK(pad_wire_decode(wire, sizeof(wire), &dec) == 0);
    CHECK(dec.report_id == 7);
    CHECK(pad_wire_buttons_to_ctrl(dec.buttons) == (PAD_WIRE_CTRL_CROSS | PAD_WIRE_CTRL_LTRIGGER));
    CHECK(pad_wire_stick_to_u8(dec.left_x) == 0 && pad_wire_stick_to_u8(dec.left_y) == 255);
    CHECK(dec.l2 == 200 && dec.r2 == 0);

    // Disconnected / intercepted -> neutral.
    s.connected = 0;
    vd_ds4_make_report(&s, 8, 0, &r);
    CHECK(r.buttons == 0 && r.left_x == 0 && r.l2 == 0);

    // Touch records: built as the shared VdTouchReport, serialized by the core
    // (vd_touch_serialize) and decoded by the Vita plugin's parser.
    s.connected = 1;
    s.buttons = 0;
    s.fingers = 2;
    s.tx[0] = 0;
    s.ty[0] = 0;
    s.tx[1] = 1919;
    s.ty[1] = 941;
    VdTouchReport tr;
    uint8_t rec[16];
    pad_wire_touch t;
    CHECK(vd_ds4_make_touch(&s, VD_TOUCH_FRONT, &tr) == 1);
    CHECK(vd_touch_serialize(&tr, rec) == 16);
    CHECK(pad_wire_touch_decode(rec, 16, &t) == 0);
    CHECK(t.port == 0 && t.count == 2 && t.f0_active && t.f1_active);
    CHECK(t.f0_x == 0 && t.f0_y == 0 && t.f1_x == 1919 && t.f1_y == 1087);
    // Same bytes as the Vita-side encoder (one wire format, two encoders).
    uint8_t rec2[16];
    CHECK(pad_wire_touch_encode(&t, rec2, 16) == 16 && memcmp(rec, rec2, 16) == 0);
    s.tx[0] = 5000; // out-of-range input is clamped, never rejected
    s.ty[0] = 5000;
    CHECK(vd_ds4_make_touch(&s, VD_TOUCH_REAR, &tr) == 1);
    CHECK(vd_touch_serialize(&tr, rec) == 16);
    CHECK(pad_wire_touch_decode(rec, 16, &t) == 0 && t.port == 1);
    s.fingers = 0;
    CHECK(vd_ds4_make_touch(&s, VD_TOUCH_REAR, &tr) == 1);
    CHECK(tr.count == 0 && !tr.f0_active);
    CHECK(vd_ds4_make_touch(&s, VD_TOUCH_OFF, &tr) == 0);
    VdTouchReport rel, rel2;
    vd_touch_release(VD_TOUCH_FRONT, &rel);
    CHECK(rel.port == 0 && rel.count == 0);
    vd_touch_release(VD_TOUCH_REAR, &rel2);
    CHECK(rel2.port == 1 && !vd_touch_same(&rel, &rel2));
    VdTouchReport back;
    CHECK(vd_touch_serialize(&rel2, rec) == 16 && vd_touch_deserialize(rec, &back) == 1 &&
          vd_touch_same(&rel2, &back));
}

// ---- libusb transfer layout detection -----------------------------------------

static void test_xfer_layout()
{
    const int n = 32;
    // Upstream libusb layout.
    {
        std::vector<uint8_t> t(60 + 12 * n, 0);
        memcpy(&t[56], &n, 4);
        VdXferLayout L;
        CHECK(vd_xfer_layout_detect(t.data(), n, &L) == VD_XFER_LAYOUT_UPSTREAM);
        uint8_t buf[16];
        int cb_token, user;
        vd_xfer_fill_iso(&L, t.data(), (void *)0x1234, 0x82, buf, n, 192, &cb_token, &user, 0);
        CHECK(t[9] == 0x82 && t[10] == 1);
        CHECK(vd_xfer_buffer(&L, t.data()) == buf);
        CHECK(vd_xfer_user_data(&L, t.data()) == &user);
        CHECK(vd_xfer_num_iso(&L, t.data()) == n);
        CHECK(vd_xfer_iso_length(&L, t.data(), 31) == 192);
        int len;
        memcpy(&len, &t[20], 4);
        CHECK(len == 192 * n);
        // Simulate completion: packet 3 got 96 bytes.
        const unsigned actual = 96;
        memcpy(&t[60 + 12 * 3 + 4], &actual, 4);
        CHECK(vd_xfer_iso_actual(&L, t.data(), 3) == 96);
    }
    // FreeBSD libusb layout.
    {
        std::vector<uint8_t> t(68 + 12 * n, 0);
        memcpy(&t[64], &n, 4);
        VdXferLayout L;
        CHECK(vd_xfer_layout_detect(t.data(), n, &L) == VD_XFER_LAYOUT_FREEBSD);
        uint8_t buf[16];
        int user;
        vd_xfer_fill_iso(&L, t.data(), nullptr, 0x82, buf, n, 192, nullptr, &user, 0);
        uint32_t ep;
        memcpy(&ep, &t[12], 4);
        CHECK(ep == 0x82 && t[16] == 1);
        CHECK(vd_xfer_buffer(&L, t.data()) == buf);
        CHECK(vd_xfer_num_iso(&L, t.data()) == n);
        CHECK(vd_xfer_iso_length(&L, t.data(), 0) == 192);
    }
    // Unknown -> refused.
    {
        std::vector<uint8_t> t(512, 0);
        VdXferLayout L;
        CHECK(vd_xfer_layout_detect(t.data(), n, &L) == VD_XFER_LAYOUT_NONE);
        CHECK(vd_xfer_layout_detect(nullptr, n, &L) == VD_XFER_LAYOUT_NONE);
    }
}

// ---- audio ring ----------------------------------------------------------------

static void test_audio_ring()
{
    static vd4::AudioRing ring;
    ring.reset();
    int16_t in[48 * 2], out[256 * 2];
    for (int i = 0; i < 48; ++i)
    {
        in[i * 2] = (int16_t)i;
        in[i * 2 + 1] = (int16_t)-i;
    }
    CHECK(ring.push(in, 48) == 48);
    CHECK(ring.available() == 48);
    CHECK(ring.pop(out, 256) == 48);
    CHECK(out[10] == 5 && out[11] == -5);
    // Overflow drops, trim caps latency.
    uint32_t pushed = 0;
    for (int i = 0; i < 1000; ++i)
        pushed += ring.push(in, 48);
    CHECK(pushed == vd4::AudioRing::kFrames);
    CHECK(ring.trim(1536) == vd4::AudioRing::kFrames - 1536);
    CHECK(ring.available() == 1536);

    // Concurrent producer/consumer keeps sample order.
    ring.reset();
    std::atomic<bool> done{false};
    std::thread prod([&] {
        int16_t buf[48 * 2];
        int v = 0;
        for (int round = 0; round < 20000; ++round)
        {
            for (int i = 0; i < 48; ++i)
            {
                buf[i * 2] = (int16_t)(v + i);
                buf[i * 2 + 1] = 0;
            }
            uint32_t n = ring.push(buf, 48);
            v += (int)n;
            if (n < 48)
                std::this_thread::yield();
            // Resend the remainder so the stream stays continuous.
            while (n < 48)
            {
                uint32_t m = ring.push(buf + n * 2, 48 - n);
                n += m;
                v += (int)m;
            }
        }
        done = true;
    });
    int16_t expect = 0;
    bool ordered = true;
    while (!done.load() || ring.available())
    {
        uint32_t n = ring.pop(out, 256);
        for (uint32_t i = 0; i < n; ++i)
        {
            ordered &= out[i * 2] == expect;
            ++expect;
        }
    }
    prod.join();
    CHECK(ordered);
}


// ---- upscaler: checked against a direct port of VITA5's shaders -------------

namespace upref
{
// Straight transcription of VITA5 upscale_shaders.hpp (kYuv, kBlit, kEasu,
// kRcas) on float images with texelFetch(clamp(p)) semantics. RCAS uses
// ffx_fsr1.h's limiter sign (min(4*mn4 - 4, -eps)); see upscale.c.
struct Img
{
    int w = 0, h = 0;
    std::vector<float> r, g, b;
    void init(int W_, int H_)
    {
        w = W_;
        h = H_;
        r.assign((size_t)w * h, 0);
        g.assign((size_t)w * h, 0);
        b.assign((size_t)w * h, 0);
    }
    size_t at(int x, int y) const
    {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return (size_t)y * w + x;
    }
};

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void yuv(const uint8_t *nv12, int w, int h, Img &o)
{
    o.init(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float yv = nv12[(size_t)y * w + x] - 16.0f;
            const uint8_t *uv = nv12 + (size_t)w * h + (size_t)(y / 2) * w + (x / 2) * 2;
            const float u = uv[0] - 128.0f, v = uv[1] - 128.0f, c = 298.0f * yv;
            const size_t i = (size_t)y * w + x;
            o.r[i] = clampf((c + 409.0f * v + 128.0f) / 256.0f, 0, 255) / 255.0f;
            o.g[i] = clampf((c - 100.0f * u - 208.0f * v + 128.0f) / 256.0f, 0, 255) / 255.0f;
            o.b[i] = clampf((c + 516.0f * u + 128.0f) / 256.0f, 0, 255) / 255.0f;
        }
}

static float luma2(const Img &s, size_t i)
{
    return s.b[i] * 0.5f + (s.r[i] * 0.5f + s.g[i]);
}

static void blit(const Img &s, Img &o)
{
    o.init(s.w * 2, s.h * 2);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
        {
            const float px = (x + 0.5f) * 0.5f - 0.5f, py = (y + 0.5f) * 0.5f - 0.5f;
            const int x0 = (int)floorf(px), y0 = (int)floorf(py);
            const float ax = px - x0, ay = py - y0;
            const size_t a = s.at(x0, y0), b = s.at(x0 + 1, y0), c = s.at(x0, y0 + 1),
                         d = s.at(x0 + 1, y0 + 1);
            const size_t i = (size_t)y * o.w + x;
            auto mix = [&](const std::vector<float> &p) {
                return p[a] * (1 - ax) * (1 - ay) + p[b] * ax * (1 - ay) + p[c] * (1 - ax) * ay +
                       p[d] * ax * ay;
            };
            o.r[i] = mix(s.r);
            o.g[i] = mix(s.g);
            o.b[i] = mix(s.b);
        }
}

static void easu_set(float &dx, float &dy, float &len, float w, float lA, float lB, float lC,
                     float lD, float lE)
{
    float lenX = fmaxf(fabsf(lD - lC), fabsf(lC - lB));
    lenX = 1.0f / fmaxf(lenX, 1e-6f);
    const float dirX = lD - lB;
    dx += dirX * w;
    lenX = clampf(fabsf(dirX) * lenX, 0, 1);
    lenX *= lenX;
    len += lenX * w;
    float lenY = fmaxf(fabsf(lE - lC), fabsf(lC - lA));
    lenY = 1.0f / fmaxf(lenY, 1e-6f);
    const float dirY = lE - lA;
    dy += dirY * w;
    lenY = clampf(fabsf(dirY) * lenY, 0, 1);
    lenY *= lenY;
    len += lenY * w;
}

static void easu(const Img &s, Img &o)
{
    o.init(s.w * 2, s.h * 2);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
        {
            const float pfx = (x + 0.5f) * 0.5f - 0.5f, pfy = (y + 0.5f) * 0.5f - 0.5f;
            const int fx = (int)floorf(pfx), fy = (int)floorf(pfy);
            const float ppx = pfx - fx, ppy = pfy - fy;
            const int ox[12] = {0, 1, -1, 0, 0, -1, 1, 2, 2, 1, 1, 0};
            const int oy[12] = {-1, -1, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2};
            size_t t[12];
            float L[12];
            for (int k = 0; k < 12; ++k)
            {
                t[k] = s.at(fx + ox[k], fy + oy[k]);
                L[k] = luma2(s, t[k]);
            }
            // b c i j f e k l h g o n
            const float bL = L[0], cL = L[1], iL = L[2], jL = L[3], fL = L[4], eL = L[5],
                        kL = L[6], lL = L[7], hL = L[8], gL = L[9], oL = L[10], nL = L[11];
            float dx = 0, dy = 0, len = 0;
            easu_set(dx, dy, len, (1 - ppx) * (1 - ppy), bL, eL, fL, gL, jL);
            easu_set(dx, dy, len, ppx * (1 - ppy), cL, fL, gL, hL, kL);
            easu_set(dx, dy, len, (1 - ppx) * ppy, fL, iL, jL, kL, nL);
            easu_set(dx, dy, len, ppx * ppy, gL, jL, kL, lL, oL);
            float dirR = dx * dx + dy * dy;
            const bool zro = dirR < 1.0f / 32768.0f;
            dirR = 1.0f / sqrtf(fmaxf(dirR, 1e-12f));
            dirR = zro ? 1.0f : dirR;
            dx = zro ? 1.0f : dx;
            dx *= dirR;
            dy *= dirR;
            len = len * 0.5f;
            len *= len;
            const float stretch = (dx * dx + dy * dy) / fmaxf(fabsf(dx), fabsf(dy));
            const float l2x = 1.0f + (stretch - 1.0f) * len, l2y = 1.0f - 0.5f * len;
            const float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
            const float clp = 1.0f / fmaxf(lob, 1e-6f);
            float ac[3] = {0, 0, 0}, aw = 0;
            for (int k = 0; k < 12; ++k)
            {
                const float offx = ox[k] - ppx, offy = oy[k] - ppy;
                float vx = offx * dx + offy * dy, vy = offx * (-dy) + offy * dx;
                vx *= l2x;
                vy *= l2y;
                const float d2 = fminf(vx * vx + vy * vy, clp);
                float wB = 0.4f * d2 - 1.0f, wA = lob * d2 - 1.0f;
                wB *= wB;
                wA *= wA;
                wB = (25.0f / 16.0f) * wB - (25.0f / 16.0f - 1.0f);
                const float wt = wB * wA;
                ac[0] += s.r[t[k]] * wt;
                ac[1] += s.g[t[k]] * wt;
                ac[2] += s.b[t[k]] * wt;
                aw += wt;
            }
            const std::vector<float> *ch[3] = {&s.r, &s.g, &s.b};
            std::vector<float> *oc[3] = {&o.r, &o.g, &o.b};
            const size_t i = (size_t)y * o.w + x;
            for (int c = 0; c < 3; ++c)
            {
                const std::vector<float> &p = *ch[c];
                const float mn = fminf(fminf(p[t[4]], p[t[9]]), fminf(p[t[3]], p[t[6]]));
                const float mx = fmaxf(fmaxf(p[t[4]], p[t[9]]), fmaxf(p[t[3]], p[t[6]]));
                (*oc[c])[i] = fminf(mx, fmaxf(mn, ac[c] / fmaxf(aw, 1e-6f)));
            }
        }
}

static void rcas(const Img &s, std::vector<uint32_t> &out)
{
    out.assign((size_t)s.w * s.h, 0);
    const float limit = 0.25f - 1.0f / 16.0f;
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x)
        {
            const size_t b = s.at(x, y - 1), d = s.at(x - 1, y), e = s.at(x, y),
                         f = s.at(x + 1, y), h = s.at(x, y + 1);
            const std::vector<float> *ch[3] = {&s.r, &s.g, &s.b};
            float lobe = -1e30f;
            for (int c = 0; c < 3; ++c)
            {
                const std::vector<float> &p = *ch[c];
                const float mn4 = fminf(fminf(p[b], p[d]), fminf(p[f], p[h]));
                const float mx4 = fmaxf(fmaxf(p[b], p[d]), fmaxf(p[f], p[h]));
                const float hitMin = fminf(mn4, p[e]) / fmaxf(4.0f * mx4, 1e-6f);
                const float hitMax = (1.0f - fmaxf(mx4, p[e])) / fminf(4.0f * mn4 - 4.0f, -1e-6f);
                lobe = fmaxf(lobe, fmaxf(-hitMin, hitMax));
            }
            const float w = fmaxf(-limit, fminf(lobe, 0.0f));
            const float rcpL = 1.0f / (4.0f * w + 1.0f);
            uint32_t px = 0xFF000000u;
            for (int c = 0; c < 3; ++c)
            {
                const std::vector<float> &p = *ch[c];
                const float v = (w * (p[b] + p[d] + p[f] + p[h]) + p[e]) * rcpL;
                const int q = (int)(clampf(v, 0, 1) * 255.0f + 0.5f);
                px |= (uint32_t)q << (16 - 8 * c);
            }
            out[(size_t)y * s.w + x] = px;
        }
}
} // namespace upref

static int px_diff(uint32_t a, uint32_t b)
{
    int m = 0;
    for (int s = 0; s < 24; s += 8)
    {
        const int d = abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF));
        m = d > m ? d : m;
    }
    return m;
}

static void make_test_frame(std::vector<uint8_t> &nv, int w, int h, unsigned seed)
{
    nv.assign((size_t)w * h * 3 / 2, 0);
    srand(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            int v = 16 + (x * 3 + y * 2) % 200;            // ramps
            if ((x / 7 + y / 5) % 3 == 0)                  // hard edges
                v = 235;
            if (x > w / 2 && y > h / 2)                    // noise quadrant
                v = 16 + rand() % 220;
            nv[(size_t)y * w + x] = (uint8_t)v;
        }
    for (size_t i = (size_t)w * h; i < nv.size(); ++i)
        nv[i] = (uint8_t)(64 + rand() % 128);
}

static void test_upscale()
{
    CHECK(strcmp(vd_upscale_label(VD_UPSCALE_OFF), "Off") == 0);
    CHECK(strcmp(vd_upscale_label(VD_UPSCALE_SHARP), "Sharp") == 0);
    CHECK(strcmp(vd_upscale_label(VD_UPSCALE_FSR), "FSR") == 0);
    CHECK(vd_upscaler_create(1, 10) == nullptr);

    // Against the shader transcription (VITA5's RGB chain), every pixel
    // including the borders. VITA4 runs EASU/RCAS on luma, so a grey picture
    // must match the RGB chain; a colour one differs only in colour detail
    // the 4:2:0 video never had (checked on average).
    const int w = 96, h = 64;
    std::vector<uint8_t> nv, grey;
    make_test_frame(nv, w, h, 7);
    grey = nv;
    memset(grey.data() + (size_t)w * h, 128, (size_t)w * h / 2);
    // Colour: a smooth colour picture (real 4:2:0 video carries no per-pixel
    // colour detail): must match VITA5's RGB chain within a level.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            nv[(size_t)y * w + x] = (uint8_t)(60 + x + y);
    for (int cy = 0; cy < h / 2; ++cy)
        for (int cx = 0; cx < w / 2; ++cx)
        {
            nv[(size_t)w * h + (size_t)cy * w + 2 * cx] = (uint8_t)(100 + cx);
            nv[(size_t)w * h + (size_t)cy * w + 2 * cx + 1] = (uint8_t)(110 + cy);
        }
    VdUpscaler *u = vd_upscaler_create(w, h);
    CHECK(u != nullptr);
    for (int pass = 0; pass < 2; ++pass)
    for (int mode = VD_UPSCALE_SHARP; mode <= VD_UPSCALE_FSR; ++mode)
    {
        const std::vector<uint8_t> &frame = pass == 0 ? grey : nv;
        std::vector<uint32_t> got((size_t)w * h * 4, 0);
        CHECK(vd_upscale_frame(u, frame.data(), mode, got.data(), w * 2, 0, h * 2) == 0);
        upref::Img src, big;
        upref::yuv(frame.data(), w, h, src);
        if (mode == VD_UPSCALE_FSR)
            upref::easu(src, big);
        else
            upref::blit(src, big);
        std::vector<uint32_t> want;
        upref::rcas(big, want);
        int inner = 0, edge = 0, off = 0;
        double sum = 0;
        for (int y = 0; y < h * 2; ++y)
            for (int x = 0; x < w * 2; ++x)
            {
                const int d = px_diff(got[(size_t)y * w * 2 + x], want[(size_t)y * w * 2 + x]);
                sum += d;
                off += d > 2 && !(x < 4 || y < 4 || x >= w * 2 - 4 || y >= h * 2 - 4);
                const bool border = x < 4 || y < 4 || x >= w * 2 - 4 || y >= h * 2 - 4;
                int &m = border ? edge : inner;
                m = d > m ? d : m;
            }
        const double mean = sum / (w * h * 4);
        printf("upscale %s (%s) vs VITA5's RGB shaders: max diff %d inside, %d at the border, "
               "mean %.2f\n",
               vd_upscale_label(mode), pass == 0 ? "grey" : "colour", inner, edge, mean);
        if (pass == 0)
        {
            // Grey: identical but for rare pixels where FSR's own direction
            // cut-off (dirR < 1/32768) flips on rounding, and the outer ring.
            CHECK(off * 1000 <= w * h * 4); // > 99.9% within 2 levels
            CHECK(mean < 0.25);
            if (mode == VD_UPSCALE_SHARP)
                CHECK(inner <= 2 && edge <= 2);
        }
        else
            CHECK(inner <= 2 && edge <= 2 && mean < 1.0); // rounding only
        if (pass == 0)
            continue;

        // Row bands of any size give the same picture as one pass.
        std::vector<uint32_t> banded((size_t)w * h * 4, 0);
        vd_upscale_convert(u, nv.data(), 0, 9);
        vd_upscale_convert(u, nv.data(), 9, h);
        if (mode == VD_UPSCALE_FSR)
        {
            vd_upscale_analyze(u, 0, 1);
            vd_upscale_analyze(u, 1, 40);
            vd_upscale_analyze(u, 40, h);
        }
        const int cuts[] = {0, 1, 2, 37, 38, 100, h * 2};
        for (int i = 0; i + 1 < 7; ++i)
            vd_upscale_render(u, mode, banded.data(), w * 2, 0, h * 2, cuts[i], cuts[i + 1]);
        CHECK(banded == got);

        // Crop: rows offset by 4, 8 fewer rows.
        std::vector<uint32_t> crop((size_t)w * 2 * (h * 2 - 8), 0);
        vd_upscale_render(u, mode, crop.data(), w * 2, 4, h * 2 - 8, 0, h * 2 - 8);
        CHECK(memcmp(crop.data(), got.data() + (size_t)4 * w * 2, crop.size() * 4) == 0);
    }

    // A flat picture stays flat (no ringing from the sharpener).
    std::vector<uint8_t> flat((size_t)w * h * 3 / 2, 128);
    memset(flat.data(), 120, (size_t)w * h);
    for (int mode = VD_UPSCALE_SHARP; mode <= VD_UPSCALE_FSR; ++mode)
    {
        std::vector<uint32_t> o((size_t)w * h * 4, 0);
        vd_upscale_frame(u, flat.data(), mode, o.data(), w * 2, 0, h * 2);
        bool same = true;
        for (uint32_t p : o)
            same = same && p == o[0];
        CHECK(same && px_diff(o[0], vd_nv12_pixel_ref(flat.data(), w, h, 0, 0)) <= 1);
        if (!same || px_diff(o[0], vd_nv12_pixel_ref(flat.data(), w, h, 0, 0)) > 1)
            printf("flat: same=%d %08x vs %08x\n", same, o[0], vd_nv12_pixel_ref(flat.data(), w, h, 0, 0));
    }
    vd_upscaler_destroy(u);
}

// Spread (standard deviation) of one 8-bit channel over the inner area.
static double channel_spread(const std::vector<uint32_t> &px, int w, int h)
{
    double sum = 0, sum2 = 0;
    int n = 0;
    for (int y = 8; y < h - 8; ++y)
        for (int x = 8; x < w - 8; ++x)
        {
            const double v = (px[(size_t)y * w + x] >> 8) & 0xFF;
            sum += v;
            sum2 += v * v;
            ++n;
        }
    const double mean = sum / n;
    return sqrt(sum2 / n - mean * mean);
}

static void test_upscale_sharpness()
{
    const int w = 96, h = 64;
    std::vector<uint8_t> nv;
    make_test_frame(nv, w, h, 7);
    memset(nv.data() + (size_t)w * h, 128, (size_t)w * h / 2); // grey
    VdUpscaler *u = vd_upscaler_create(w, h);

    // Sharpness off: the plain 2x picture (bilinear here), no RCAS at all.
    std::vector<uint32_t> off((size_t)w * h * 4);
    vd_upscaler_set_sharpness(u, 0.0f, 0);
    vd_upscale_frame(u, nv.data(), VD_UPSCALE_SHARP, off.data(), w * 2, 0, h * 2);
    upref::Img src, big;
    upref::yuv(nv.data(), w, h, src);
    upref::blit(src, big);
    int worst = 0;
    for (size_t i = 0; i < off.size(); ++i)
    {
        const int want = (int)(upref::clampf(big.g[i], 0, 1) * 255.0f + 0.5f);
        const int d = abs((int)((off[i] >> 8) & 0xFF) - want);
        worst = d > worst ? d : worst;
    }
    CHECK(worst <= 1);

    // FSR 2's slider: more sharpness, further from the unsharpened picture.
    double prev = -1;
    bool rising = true;
    for (int level = 1; level <= 4; ++level)
    {
        std::vector<uint32_t> o((size_t)w * h * 4);
        vd_upscaler_set_sharpness(u, level / 4.0f, 0);
        vd_upscale_frame(u, nv.data(), VD_UPSCALE_SHARP, o.data(), w * 2, 0, h * 2);
        double dev = 0;
        for (size_t i = 0; i < o.size(); ++i)
            dev += abs((int)((o[i] >> 8) & 0xFF) - (int)((off[i] >> 8) & 0xFF));
        rising = rising && dev > prev;
        prev = dev;
    }
    CHECK(rising);

    // Denoise (FSR_RCAS_DENOISE, on in FSR 2): fine grain on a flat grey is
    // sharpened less, so it spreads less than without it - but more than off.
    std::vector<uint8_t> grain((size_t)w * h * 3 / 2, 128);
    srand(3);
    for (int i = 0; i < w * h; ++i)
        grain[i] = (uint8_t)(100 + rand() % 7);
    double spread[3];
    const float sharp[3] = {0.0f, 1.0f, 1.0f};
    const int dn[3] = {0, 1, 0};
    for (int k = 0; k < 3; ++k)
    {
        std::vector<uint32_t> o((size_t)w * h * 4);
        vd_upscaler_set_sharpness(u, sharp[k], dn[k]);
        vd_upscale_frame(u, grain.data(), VD_UPSCALE_FSR, o.data(), w * 2, 0, h * 2);
        spread[k] = channel_spread(o, w * 2, h * 2);
    }
    printf("grain spread: sharpening off %.2f, max + denoise %.2f, max %.2f\n", spread[0],
           spread[1], spread[2]);
    CHECK(spread[0] < spread[1] && spread[1] < spread[2]);
    vd_upscaler_destroy(u);
}

static void test_smooth()
{
    const int w = 50, h = 30; // odd sizes reach every scalar edge path
    std::vector<uint32_t> src((size_t)w * h);
    srand(5);
    for (auto &p : src)
        p = 0xFF000000u | (uint32_t)(rand() & 0xFFFFFF);

    // Sharpen: k = 0 copies; SIMD matches the per-channel definition.
    std::vector<uint32_t> sh((size_t)w * h), ref((size_t)w * h);
    vd_smooth_sharpen_rows(src.data(), sh.data(), w, h, 0, 0, h);
    CHECK(sh == src);
    const int k = 28;
    vd_smooth_sharpen_rows(src.data(), sh.data(), w, h, k, 0, 7);
    vd_smooth_sharpen_rows(src.data(), sh.data(), w, h, k, 7, h); // bands
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            auto at = [&](int xx, int yy) {
                xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
                yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
                return src[(size_t)yy * w + xx];
            };
            uint32_t out = 0xFF000000u;
            for (int s = 0; s < 24; s += 8)
            {
                const int c = (at(x, y) >> s) & 0xFF;
                const int sum = ((at(x, y - 1) >> s) & 0xFF) + ((at(x, y + 1) >> s) & 0xFF) +
                                ((at(x - 1, y) >> s) & 0xFF) + ((at(x + 1, y) >> s) & 0xFF);
                int v = c + (((4 * c - sum) * k + 128) >> 8);
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                out |= (uint32_t)v << s;
            }
            ref[(size_t)y * w + x] = out;
        }
    CHECK(sh == ref);

    // 2x: GL-style bilinear (clamp to edge) within one level, every pixel.
    std::vector<uint32_t> out((size_t)w * 2 * h * 2, 0), scratch(w + 2);
    vd_smooth_render_rows(src.data(), w, h, out.data(), w * 2, 0, h * 2, 0, 9, scratch.data());
    vd_smooth_render_rows(src.data(), w, h, out.data(), w * 2, 0, h * 2, 9, h * 2,
                          scratch.data());
    int worst = 0;
    double bias = 0;
    bool alpha_ok = true;
    for (int y = 0; y < h * 2; ++y)
        for (int x = 0; x < w * 2; ++x)
        {
            const float px = (x + 0.5f) * 0.5f - 0.5f, py = (y + 0.5f) * 0.5f - 0.5f;
            const int x0 = (int)floorf(px), y0 = (int)floorf(py);
            const float ax = px - x0, ay = py - y0;
            auto at = [&](int xx, int yy) {
                xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
                yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
                return src[(size_t)yy * w + xx];
            };
            for (int s = 0; s < 24; s += 8)
            {
                auto ch = [&](uint32_t p) { return (float)((p >> s) & 0xFF); };
                const float want = ch(at(x0, y0)) * (1 - ax) * (1 - ay) +
                                   ch(at(x0 + 1, y0)) * ax * (1 - ay) +
                                   ch(at(x0, y0 + 1)) * (1 - ax) * ay +
                                   ch(at(x0 + 1, y0 + 1)) * ax * ay;
                const float got = (float)((out[(size_t)y * w * 2 + x] >> s) & 0xFF);
                const int d = (int)lroundf(fabsf(got - want));
                worst = d > worst ? d : worst;
                bias += got - want;
            }
            alpha_ok = alpha_ok && (out[(size_t)y * w * 2 + x] >> 24) == 0xFF;
        }
    bias /= (double)w * h * 4 * 3;
    printf("smooth 2x vs bilinear: worst %d levels, mean bias %+.2f\n", worst, bias);
    CHECK(worst <= 2);
    CHECK(bias > -0.5 && bias < 0.5);
    CHECK(alpha_ok);

    // Crop: offset rows match the uncropped picture.
    std::vector<uint32_t> crop((size_t)w * 2 * (h * 2 - 8));
    vd_smooth_render_rows(src.data(), w, h, crop.data(), w * 2, 4, h * 2 - 8, 0, h * 2 - 8,
                          scratch.data());
    CHECK(memcmp(crop.data(), out.data() + (size_t)4 * w * 2, crop.size() * 4) == 0);
}

static void test_upscale_worker()
{
    using vd4::UpscaleWorker;
    CHECK(UpscaleWorker::supported(960, 544));
    CHECK(!UpscaleWorker::supported(640, 368));
    const int w = 960, h = 544;
    std::vector<uint8_t> nv;
    make_test_frame(nv, w, h, 11);
    VdUpscaler *u = vd_upscaler_create(w, h);
    std::vector<uint32_t> want((size_t)1920 * 1080, 0);
    vd_upscaler_set_sharpness(u, 1.0f, 1); // what the worker uses at Sharpness Max
    vd_upscale_frame(u, nv.data(), VD_UPSCALE_FSR, want.data(), 1920, 4, 1080);
    vd_upscaler_destroy(u);

    UpscaleWorker wk;
    CHECK(wk.latest() == nullptr); // not started
    CHECK(wk.start(2));
    wk.submit(nv.data(), w, h, VD_UPSCALE_OFF); // Off is never upscaled
    wk.submit(nv.data(), w, h, VD_UPSCALE_FSR);
    const uint32_t *got = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    while (!got && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30))
    {
        got = wk.latest();
        if (!got)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got != nullptr);
    CHECK(got && memcmp(got, want.data(), want.size() * 4) == 0);
    wk.reset();
    CHECK(wk.latest() == nullptr); // nothing from before the reset
    wk.stop();
}

static void test_chords()
{
    // L3 held, then the D-pad: chords on the rising edge.
    VdChordEvents e = vd_chord_events(DS4_L3 | DS4_RIGHT, DS4_L3);
    CHECK(e.upscale_cycle == 1 && !e.toggle_fullscreen);
    e = vd_chord_events(DS4_L3 | DS4_LEFT, DS4_L3);
    CHECK(e.upscale_cycle == -1);
    e = vd_chord_events(DS4_L3 | DS4_UP, DS4_L3);
    CHECK(e.toggle_fullscreen && e.upscale_cycle == 0);
    e = vd_chord_events(DS4_L3 | DS4_RIGHT, DS4_L3 | DS4_RIGHT); // held, not new
    CHECK(e.upscale_cycle == 0);
    e = vd_chord_events(DS4_RIGHT, 0); // no L3
    CHECK(e.upscale_cycle == 0 && !e.toggle_fullscreen);
    e = vd_chord_events(DS4_L3 | DS4_RIGHT | DS4_INTERCEPTED, DS4_L3); // system UI
    CHECK(e.upscale_cycle == 0);
    e = vd_chord_events(DS4_L3 | DS4_RIGHT, 0); // both at once count too
    CHECK(e.upscale_cycle == 1);

    // While L3 is held the chord buttons never reach the Vita.
    const uint32_t all = DS4_L3 | DS4_R3 | DS4_UP | DS4_DOWN | DS4_LEFT | DS4_RIGHT | DS4_CROSS;
    CHECK(vd_mask_modifiers(all) == DS4_CROSS);
    CHECK(vd_mask_modifiers(DS4_UP | DS4_CROSS) == (DS4_UP | DS4_CROSS));
    CHECK(vd_mask_modifiers(DS4_R3 | DS4_LEFT) == (DS4_R3 | DS4_LEFT));
}

// ---- canvas + settings -------------------------------------------------------

static void test_canvas_settings()
{
    std::vector<uint32_t> px(64 * 64, 0xFF000000u);
    vd4::Canvas c(px.data(), 64, 64, 64);
    c.blend_rect(-10, -10, 200, 200, 0xFFFFFF, 128); // clipped
    CHECK((px[0] & 0xFF) == 128 || (px[0] & 0xFF) == 127);
    c.round_rect(-20, -20, 300, 300, 30, 0x00FF00);
    c.circle(70, 70, 30, 0xFF0000);
    c.line(-100, -100, 200, 200, 3, 0x0000FF);
    c.round_rect_outline(2, 2, 60, 60, 12, 2, 0x123456);
    CHECK(px[32 * 64 + 32] != 0);

    vd4::Settings def;
    CHECK(def.touch_target == VD_TOUCH_FRONT && def.passthrough); // VITA5 beta defaults
    vd4::Settings s;
    s.touch_target = VD_TOUCH_REAR;
    s.scale_mode = 2;
    s.sound = false;
    s.stats = true;
    s.passthrough = false;
    s.upscale = VD_UPSCALE_FSR;
    s.sharpness = 2;
    s.taps = VD_TAPS_RAW;
    s.low_lag = true;
    s.fast_picture = true;
    s.deadzone = false;
    const char *path = "/tmp/vita4_settings_test.txt";
    CHECK(vd4::settings_save(path, s));
    vd4::Settings r;
    CHECK(vd4::settings_load(path, r));
    CHECK(r.touch_target == VD_TOUCH_REAR && r.scale_mode == 2 && !r.sound && r.stats &&
          !r.passthrough); // a saved (version 2) rear choice is kept
    CHECK(r.upscale == VD_UPSCALE_FSR);
    CHECK(def.upscale == VD_UPSCALE_OFF); // off by default, as VITA5
    CHECK(def.sharpness == 4 && r.sharpness == 2); // max by default (VITA5 strength)
    CHECK(def.taps == VD_TAPS_STEADY && r.taps == VD_TAPS_RAW);
    CHECK(!def.low_lag && r.low_lag); // Off by default (2.0), a choice is kept
    CHECK(!def.fast_picture && r.fast_picture); // 1.40: Fast available again (default Full)
    CHECK(def.deadzone && !r.deadzone);
    {
        FILE *o = fopen(path, "w");
        fputs("version=2\nlowlag=0\nlowlag2=0\n", o); // older Low lag versions
        fclose(o);
        vd4::Settings old;
        CHECK(vd4::settings_load(path, old) && !old.low_lag); // ignored: the default
    }
    FILE *f = fopen(path, "w");
    fputs("touch=0\npicture=1\nsound=1\nstats=0\n", f); // 1.04 file: old rear default
    fclose(f);
    vd4::Settings mig;
    CHECK(vd4::settings_load(path, mig));
    CHECK(mig.touch_target == VD_TOUCH_FRONT && mig.scale_mode == 1 && mig.passthrough);
    f = fopen(path, "w");
    fputs("touch=99\npicture=-1\nupscale=9\nbogus\n", f);
    fclose(f);
    vd4::Settings d;
    CHECK(vd4::settings_load(path, d));
    CHECK(d.touch_target == VD_TOUCH_FRONT && d.scale_mode == 0 &&
          d.upscale == VD_UPSCALE_OFF); // out of range ignored
    remove(path);
}

static void test_taps()
{
    VdTapShaper t;
    VdTapParams p;
    vd_tap_params(VD_TAPS_STEADY, &p);
    auto at = [&](int mode, int fingers, int x, int y, double now) {
        Ds4State s{};
        s.connected = 1;
        s.fingers = (uint8_t)fingers;
        s.tx[0] = (uint16_t)x;
        s.ty[0] = (uint16_t)y;
        vd_tap_apply(&t, mode, &s, now);
        return s;
    };
    const int M = VD_TAPS_STEADY;

    // Raw passes everything through untouched.
    vd_tap_reset(&t);
    Ds4State r = at(VD_TAPS_RAW, 1, 123, 456, 0.0);
    CHECK(r.fingers == 1 && r.tx[0] == 123 && r.ty[0] == 456);
    CHECK(at(VD_TAPS_RAW, 0, 0, 0, 0.004).fingers == 0);

    // A 12 ms tap (shorter than a 30 fps game frame) is shown for min_hold.
    vd_tap_reset(&t);
    CHECK(at(M, 1, 500, 300, 1.000).fingers == 1);
    CHECK(at(M, 1, 503, 301, 1.008).tx[0] == 500); // wobble: pinned
    double now = 1.012;
    bool held = true;
    for (; now < 1.000 + p.min_hold - 0.001; now += 0.004)
    {
        Ds4State s = at(M, 0, 0, 0, now);
        held = held && s.fingers == 1 && s.tx[0] == 500 && s.ty[0] == 300;
    }
    CHECK(held);
    CHECK(at(M, 0, 0, 0, 1.000 + p.min_hold + 0.001).fingers == 0);

    // Lift-off jump: one sample far away, then the lift. The tap stays put
    // and releases where it landed.
    vd_tap_reset(&t);
    at(M, 1, 800, 400, 2.000);
    for (double x = 2.004; x < 2.100; x += 0.004)
        at(M, 1, 805, 402, x);
    Ds4State j = at(M, 1, 800 + 200, 400 + 90, 2.100); // jump
    CHECK(j.fingers == 1 && j.tx[0] == 800 && j.ty[0] == 400);
    j = at(M, 0, 0, 0, 2.104 + p.dropout); // lifted (past the dropout window)
    CHECK(j.fingers == 0);

    // A real drag follows once past the slop for two samples.
    vd_tap_reset(&t);
    at(M, 1, 100, 100, 3.000);
    CHECK(at(M, 1, 100 + p.slop + 20, 100, 3.004).tx[0] == 100);
    CHECK(at(M, 1, 100 + p.slop + 30, 100, 3.008).tx[0] == 100 + p.slop + 30);
    CHECK(at(M, 1, 400, 120, 3.012).tx[0] == 400 && t.moving[0]);
    CHECK(at(M, 0, 0, 0, 3.200).fingers == 0);

    // A short dropout mid-press is not a release.
    vd_tap_reset(&t);
    at(M, 1, 600, 600, 4.000);
    at(M, 1, 600, 600, 4.100);
    CHECK(at(M, 0, 0, 0, 4.104).fingers == 1);
    CHECK(at(M, 1, 602, 600, 4.112).fingers == 1 && t.down_since == 4.000);

    // Two quick taps stay two taps: the second waits out the gap, then shows
    // (at its own position) even though the finger has already lifted.
    vd_tap_reset(&t);
    at(M, 1, 200, 200, 5.000);
    const double up = 5.000 + p.min_hold + 0.001;
    CHECK(at(M, 0, 0, 0, up).fingers == 0);
    const double second = up + p.gap / 3;
    CHECK(at(M, 1, 900, 500, second).fingers == 0);           // waiting out the gap
    CHECK(at(M, 0, 0, 0, second + 0.008 + p.dropout).fingers == 0); // lifted, still queued
    Ds4State q = at(M, 0, 0, 0, up + p.gap + 0.001);
    CHECK(q.fingers == 1 && q.tx[0] == 900 && q.ty[0] == 500);  // shown, not lost
    CHECK(at(M, 0, 0, 0, up + p.gap + 0.002 + p.min_hold).fingers == 0);

    // Two fingers.
    vd_tap_reset(&t);
    Ds4State two{};
    two.fingers = 2;
    two.tx[0] = 300;
    two.tx[1] = 1500;
    two.ty[0] = two.ty[1] = 400;
    vd_tap_apply(&t, M, &two, 6.0);
    CHECK(two.fingers == 2 && two.tx[1] == 1500);

    // Slow games: longer hold and gap.
    VdTapParams sp;
    vd_tap_params(VD_TAPS_SLOW, &sp);
    CHECK(sp.min_hold > p.min_hold && sp.gap > p.gap);
    vd_tap_reset(&t);
    at(VD_TAPS_SLOW, 1, 10, 10, 7.0);
    CHECK(at(VD_TAPS_SLOW, 0, 0, 0, 7.0 + p.min_hold + 0.01).fingers == 1);
    CHECK(at(VD_TAPS_SLOW, 0, 0, 0, 7.0 + sp.min_hold + 0.01).fingers == 0);
}

static void test_touch_alt()
{
    VdTouchAlt a;
    vd_touch_alt_reset(&a);
    auto press = [&](int fingers, uint16_t x0, uint16_t x1) {
        VdTouchReport r{};
        r.count = (uint8_t)fingers;
        if (fingers >= 1) { r.f0_active = 1; r.f0_x = x0; r.f0_y = 10; }
        if (fingers >= 2) { r.f1_active = 1; r.f1_x = x1; r.f1_y = 20; }
        vd_touch_alternate(&a, &r);
        return r;
    };
    // First tap: slot 1 (the other id from the plugin's start state)...
    VdTouchReport r = press(1, 100, 0);
    CHECK(!r.f0_active && r.f1_active && r.f1_x == 100 && r.f1_y == 10 && r.count == 1);
    r = press(1, 120, 0); // held: stays in its slot
    CHECK(r.f1_active && r.f1_x == 120 && !r.f0_active);
    r = press(0, 0, 0); // release
    CHECK(!r.f0_active && !r.f1_active);
    // ...the next tap uses slot 0, the one after slot 1 again.
    r = press(1, 300, 0);
    CHECK(r.f0_active && !r.f1_active && r.f0_x == 300);
    press(0, 0, 0);
    r = press(1, 400, 0);
    CHECK(r.f1_active && r.f1_x == 400);
    // A second finger joining a swapped press takes the free slot.
    r = press(2, 400, 900);
    CHECK(r.f0_active && r.f1_active && r.f1_x == 400 && r.f0_x == 900);
    // Every record still decodes on the plugin side.
    uint8_t wire[16];
    CHECK(vd_touch_serialize(&r, wire) == 16);
    pad_wire_touch pw;
    CHECK(pad_wire_touch_decode(wire, 16, &pw) == 0);
    press(0, 0, 0);
    r = press(1, 50, 0);
    r = press(1, 50, 0);
    r = press(0, 0, 0);
    r = press(1, 60, 0);
    CHECK(vd_touch_serialize(&r, wire) == 16 && pad_wire_touch_decode(wire, 16, &pw) == 0 &&
          pw.f1_active == 1 && pw.f0_active == 0 && pw.f1_x == 60);
}

static void test_pacing()
{
    const double R = 1.0 / 60.0;
    VdLatchChoice c;
    double ph[120];
    // Arrivals clustered at 4 ms +- 1 ms: draw just after them.
    for (int i = 0; i < 120; ++i)
        ph[i] = 0.004 + 0.001 * ((i % 21) - 10) / 10.0;
    vd_latch_choose(ph, 120, R, 0.008, &c);
    CHECK(c.usable && fabs(c.centre - 0.004) < 0.0003);
    CHECK(c.latch > 0.005 && c.latch < 0.007); // after the late edge, + margin
    CHECK(c.latch + 0.008 <= R);
    // Every arrival is before the draw moment, so all go out at the next refresh.
    bool all_before = true;
    for (int i = 0; i < 120; ++i)
        all_before = all_before && ph[i] < c.latch;
    CHECK(all_before);
    // Same cluster but a long draw: no room before the refresh -> 1.21 timing.
    vd_latch_choose(ph, 120, R, 0.013, &c);
    CHECK(!c.usable && c.latch == 0);
    // Spread over the whole refresh (irregular arrivals): 1.21 timing.
    for (int i = 0; i < 120; ++i)
        ph[i] = R * i / 120.0;
    vd_latch_choose(ph, 120, R, 0.006, &c);
    CHECK(!c.usable && c.latch == 0 && c.spread > VD_LATCH_MAX_SPREAD_S);
    // A cluster straddling the flip (16.2 .. 0.5 ms): circular maths keeps it
    // one cluster; the draw lands just after it, early in the refresh.
    for (int i = 0; i < 120; ++i)
    {
        double v = 0.0163 + 0.0009 * (i % 10) / 9.0; // 16.3 .. 17.2 ms
        ph[i] = v >= R ? v - R : v;
    }
    vd_latch_choose(ph, 120, R, 0.008, &c);
    CHECK(c.usable && c.latch > 0.0005 && c.latch < 0.003);
    // Too few samples: nothing decided.
    vd_latch_choose(ph, 4, R, 0.008, &c);
    CHECK(!c.usable && c.latch == 0);
}

static void test_deadzone()
{
    auto dz = [](int x, int y) {
        uint8_t a = (uint8_t)x, b = (uint8_t)y;
        vd_stick_deadzone(&a, &b);
        return std::pair<int, int>(a, b);
    };
    // The user's resting readings become exact centre.
    CHECK(dz(135, 132) == std::make_pair(128, 128));
    CHECK(dz(127, 125) == std::make_pair(128, 128));
    CHECK(dz(128, 128) == std::make_pair(128, 128));
    // Full pushes still reach the ends, both ways, on both axes.
    CHECK(dz(255, 128) == std::make_pair(255, 128));
    CHECK(dz(0, 128) == std::make_pair(0, 128));
    CHECK(dz(128, 0) == std::make_pair(128, 0));
    CHECK(dz(128, 255) == std::make_pair(128, 255));
    // Symmetric: equal pushes left and right give equal distances from centre.
    for (int d = 0; d <= 127; ++d)
    {
        const int r = dz(128 + d, 128).first - 128, l = 128 - dz(128 - d, 128).first;
        if (r != l)
        {
            CHECK(r == l);
            break;
        }
    }
    // Monotonic past the dead zone: more push, more output.
    int prev = 128;
    bool mono = true;
    for (int x = 128; x <= 255; ++x)
    {
        const int o = dz(x, 128).first;
        mono = mono && o >= prev;
        prev = o;
    }
    CHECK(mono);
    // Diagonal keeps its direction (x and y move together).
    const auto dg = dz(220, 36);
    CHECK(dg.first - 128 == 128 - dg.second);
}

int main()
{
    test_descriptor();
    test_scaler();
    test_pad();
    test_xfer_layout();
    test_audio_ring();
    test_canvas_settings();
    test_upscale();
    test_upscale_sharpness();
    test_smooth();
    test_upscale_worker();
    test_chords();
    test_taps();
    test_touch_alt();
    test_pacing();
    test_deadzone();
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
