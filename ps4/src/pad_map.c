// VITA4 - DS4 -> Vita mapping. See pad_map.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad_map.h"

#include <math.h>
#include <string.h>

#include "pad_wire.h"

uint32_t vd_ds4_buttons_to_vita(uint32_t b)
{
    if (b & DS4_INTERCEPTED)
        return 0;
    static const struct
    {
        uint32_t from, to;
    } kTable[] = {
        {DS4_TOUCH_PAD, VD_PAD_SELECT}, {DS4_L3, VD_PAD_L3},
        {DS4_R3, VD_PAD_R3},            {DS4_OPTIONS, VD_PAD_START},
        {DS4_UP, VD_PAD_UP},            {DS4_RIGHT, VD_PAD_RIGHT},
        {DS4_DOWN, VD_PAD_DOWN},        {DS4_LEFT, VD_PAD_LEFT},
        {DS4_L2, VD_PAD_L2},            {DS4_R2, VD_PAD_R2},
        {DS4_L1, VD_PAD_L1},            {DS4_R1, VD_PAD_R1},
        {DS4_TRIANGLE, VD_PAD_TRIANGLE}, {DS4_CIRCLE, VD_PAD_CIRCLE},
        {DS4_CROSS, VD_PAD_CROSS},      {DS4_SQUARE, VD_PAD_SQUARE},
    };
    uint32_t out = 0;
    for (unsigned i = 0; i < sizeof(kTable) / sizeof(kTable[0]); ++i)
        if (b & kTable[i].from)
            out |= kTable[i].to;
    return out;
}

int16_t vd_axis_u8_to_i16(uint8_t v)
{
    // Same mapping as the PS5 app (pad_bridge.cpp map_axis).
    int out = (int)v * 257 - 32896;
    if (out < -32768)
        out = -32768;
    if (out > 32767)
        out = 32767;
    return (int16_t)out;
}

void vd_ds4_make_report(const Ds4State *s, uint32_t report_id, uint64_t ts_us, VdPadReport *out)
{
    if (!s || !s->connected || (s->buttons & DS4_INTERCEPTED))
    {
        vd_neutral_report(report_id, ts_us, out);
        return;
    }
    vd_pad_make_report(out, report_id, ts_us, vd_ds4_buttons_to_vita(s->buttons),
                       vd_axis_u8_to_i16(s->lx), vd_axis_u8_to_i16(s->ly),
                       vd_axis_u8_to_i16(s->rx), vd_axis_u8_to_i16(s->ry), s->l2, s->r2);
}

void vd_neutral_report(uint32_t report_id, uint64_t ts_us, VdPadReport *out)
{
    vd_pad_make_report(out, report_id, ts_us, 0, 0, 0, 0, 0, 0, 0);
}

static uint16_t scale_coord(unsigned v, unsigned in_size, unsigned out_max)
{
    if (in_size < 2)
        return 0;
    if (v > in_size - 1)
        v = in_size - 1;
    return (uint16_t)((v * out_max + (in_size - 1) / 2) / (in_size - 1));
}

int vd_ds4_make_touch(const Ds4State *s, int target, VdTouchReport *out)
{
    if (target != VD_TOUCH_FRONT && target != VD_TOUCH_REAR)
        return 0;
    memset(out, 0, sizeof(*out));
    out->port = target == VD_TOUCH_FRONT ? 0u : 1u;
    unsigned fingers = (s && s->connected && !(s->buttons & DS4_INTERCEPTED)) ? s->fingers : 0;
    if (fingers > 2)
        fingers = 2;
    out->count = (uint8_t)fingers;
    if (fingers >= 1)
    {
        out->f0_active = 1;
        out->f0_x = scale_coord(s->tx[0], DS4_TOUCH_W, PAD_WIRE_TOUCH_X_MAX);
        out->f0_y = scale_coord(s->ty[0], DS4_TOUCH_H, PAD_WIRE_TOUCH_Y_MAX);
    }
    if (fingers >= 2)
    {
        out->f1_active = 1;
        out->f1_x = scale_coord(s->tx[1], DS4_TOUCH_W, PAD_WIRE_TOUCH_X_MAX);
        out->f1_y = scale_coord(s->ty[1], DS4_TOUCH_H, PAD_WIRE_TOUCH_Y_MAX);
    }
    return 1;
}

void vd_touch_release(int target, VdTouchReport *out)
{
    memset(out, 0, sizeof(*out));
    out->port = target == VD_TOUCH_FRONT ? 0u : 1u;
}

int vd_touch_same(const VdTouchReport *a, const VdTouchReport *b)
{
    return a->port == b->port && a->count == b->count && a->f0_x == b->f0_x &&
           a->f0_y == b->f0_y && a->f1_x == b->f1_x && a->f1_y == b->f1_y &&
           a->f0_active == b->f0_active && a->f1_active == b->f1_active;
}

int vd_report_same_input(const VdPadReport *a, const VdPadReport *b)
{
    return a->buttons == b->buttons && a->left_x == b->left_x && a->left_y == b->left_y &&
           a->right_x == b->right_x && a->right_y == b->right_y && a->l2 == b->l2 &&
           a->r2 == b->r2;
}

VdChordEvents vd_chord_events(uint32_t buttons, uint32_t prev)
{
    VdChordEvents e = {0, 0};
    if ((buttons & DS4_INTERCEPTED) || !(buttons & DS4_L3))
        return e;
    const uint32_t risen = buttons & ~prev;
    if (risen & DS4_RIGHT)
        e.upscale_cycle = +1;
    if (risen & DS4_LEFT)
        e.upscale_cycle = -1;
    if (risen & DS4_UP)
        e.toggle_fullscreen = 1;
    return e;
}

uint32_t vd_mask_modifiers(uint32_t buttons)
{
    if (!(buttons & DS4_L3))
        return buttons;
    return buttons & ~(DS4_L3 | DS4_R3 | DS4_UP | DS4_DOWN | DS4_LEFT | DS4_RIGHT);
}

// ---- touch tap shaping ---------------------------------------------------------

void vd_tap_params(int mode, VdTapParams *out)
{
    if (mode == VD_TAPS_SLOW)
    {
        out->min_hold = 0.130;
        out->gap = 0.080;
        out->dropout = 0.032;
        out->slop = 56;
    }
    else
    {
        out->min_hold = 0.080;
        out->gap = 0.050;
        out->dropout = 0.024;
        out->slop = 40;
    }
}

void vd_tap_reset(VdTapShaper *t)
{
    memset(t, 0, sizeof(*t));
    t->up_since = -1e9;
}

static void tap_show_press(VdTapShaper *t, uint8_t fingers, const uint16_t *x, const uint16_t *y,
                           double now)
{
    t->out_fingers = fingers;
    t->down_since = now;
    for (int i = 0; i < 2; ++i)
    {
        t->ox[i] = x[i];
        t->oy[i] = y[i];
        t->moving[i] = 0;
        t->over[i] = 0;
    }
}

void vd_tap_apply(VdTapShaper *t, int mode, Ds4State *s, double now)
{
    if (mode == VD_TAPS_RAW || mode < 0 || mode >= VD_TAPS_COUNT)
    {
        vd_tap_reset(t);
        return;
    }
    VdTapParams p;
    vd_tap_params(mode, &p);

    uint8_t rf = s->fingers > 2 ? 2 : s->fingers;
    if (rf > 0)
    {
        t->raw_fingers = rf;
        for (int i = 0; i < 2; ++i)
        {
            t->rx[i] = s->tx[i];
            t->ry[i] = s->ty[i];
        }
        t->raw_seen = now;
    }
    else if (t->raw_fingers > 0 && now - t->raw_seen < p.dropout)
        rf = t->raw_fingers; // a short "no finger" reading mid-press: still down
    else
        t->raw_fingers = 0;

    if (t->out_fingers == 0)
    {
        if (rf > 0 && !t->pending)
        {
            // A new press lands: show it now, or after the gap.
            t->pending = 1;
            t->pend_fingers = rf;
            for (int i = 0; i < 2; ++i)
            {
                t->px[i] = t->rx[i];
                t->py[i] = t->ry[i];
            }
        }
        if (t->pending && now - t->up_since >= p.gap)
        {
            t->pending = 0;
            tap_show_press(t, t->pend_fingers, t->px, t->py, now);
        }
    }
    else if (rf > 0)
    {
        // Held: a finger added or removed shows as such; positions pinned
        // until the finger really moves.
        for (int i = 0; i < 2; ++i)
        {
            if (i >= rf)
            {
                t->moving[i] = 0;
                t->over[i] = 0;
                continue;
            }
            if (i >= t->out_fingers) // second finger just landed
            {
                t->ox[i] = t->rx[i];
                t->oy[i] = t->ry[i];
                t->moving[i] = 0;
                t->over[i] = 0;
                continue;
            }
            if (!t->moving[i])
            {
                const int dx = (int)t->rx[i] - (int)t->ox[i];
                const int dy = (int)t->ry[i] - (int)t->oy[i];
                if (dx * dx + dy * dy > p.slop * p.slop)
                {
                    if (++t->over[i] >= 2)
                        t->moving[i] = 1;
                }
                else
                    t->over[i] = 0;
            }
            if (t->moving[i])
            {
                t->ox[i] = t->rx[i];
                t->oy[i] = t->ry[i];
            }
        }
        t->out_fingers = rf;
    }
    else if (now - t->down_since >= p.min_hold)
    {
        t->out_fingers = 0; // lifted, and the press was long enough: release
        t->up_since = now;
    }
    // else: lifted early, keep showing the press until min_hold

    s->fingers = t->out_fingers;
    for (int i = 0; i < 2; ++i)
    {
        s->tx[i] = t->out_fingers > i ? t->ox[i] : 0;
        s->ty[i] = t->out_fingers > i ? t->oy[i] : 0;
    }
}

void vd_touch_alt_reset(VdTouchAlt *a)
{
    memset(a, 0, sizeof(*a));
}

void vd_touch_alternate(VdTouchAlt *a, VdTouchReport *r)
{
    const int down = r->f0_active || r->f1_active;
    if (down && !a->was_down)
        a->swap ^= 1u; // a new press: the other slot (so the other id)
    a->was_down = (uint8_t)down;
    if (!a->swap)
        return;
    const uint16_t x = r->f0_x, y = r->f0_y;
    const uint8_t act = r->f0_active;
    r->f0_x = r->f1_x;
    r->f0_y = r->f1_y;
    r->f0_active = r->f1_active;
    r->f1_x = x;
    r->f1_y = y;
    r->f1_active = act;
}

// ---- stick dead zone -----------------------------------------------------------

void vd_stick_deadzone(uint8_t *x, uint8_t *y)
{
    const double dx = (double)*x - 128.0, dy = (double)*y - 128.0;
    const double mag = sqrt(dx * dx + dy * dy);
    if (mag <= VD_STICK_DEADZONE)
    {
        *x = 128;
        *y = 128;
        return;
    }
    // Map (deadzone, 128] onto (0, 128]: same direction, rescaled length.
    const double scale = (mag - VD_STICK_DEADZONE) / (128.0 - VD_STICK_DEADZONE) * 128.0 / mag;
    double ox = 128.0 + dx * scale, oy = 128.0 + dy * scale;
    ox = ox < 0 ? 0 : (ox > 255 ? 255 : ox);
    oy = oy < 0 ? 0 : (oy > 255 ? 255 : oy);
    *x = (uint8_t)(ox + 0.5 > 255 ? 255 : ox + 0.5);
    *y = (uint8_t)(oy + 0.5 > 255 ? 255 : oy + 0.5);
}
