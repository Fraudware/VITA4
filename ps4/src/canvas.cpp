// VITA4 - software 2D drawing. See canvas.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "canvas.h"

#include <math.h>
#include <string.h>

namespace vd4
{

namespace
{

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

struct RoundRect
{
    float cx, cy, hw, hh, r;
};

float sdf_round_rect(float px, float py, const void *ctx)
{
    const RoundRect &s = *static_cast<const RoundRect *>(ctx);
    const float qx = fabsf(px - s.cx) - (s.hw - s.r);
    const float qy = fabsf(py - s.cy) - (s.hh - s.r);
    const float ox = qx > 0 ? qx : 0;
    const float oy = qy > 0 ? qy : 0;
    const float outside = sqrtf(ox * ox + oy * oy);
    const float inside = (qx > qy ? qx : qy);
    return outside + (inside < 0 ? inside : 0) - s.r;
}

struct RoundRectRing
{
    RoundRect rr;
    float half;
};

float sdf_round_rect_ring(float px, float py, const void *ctx)
{
    const RoundRectRing &s = *static_cast<const RoundRectRing *>(ctx);
    return fabsf(sdf_round_rect(px, py, &s.rr)) - s.half;
}

struct Circle
{
    float cx, cy, r;
};

float sdf_circle(float px, float py, const void *ctx)
{
    const Circle &c = *static_cast<const Circle *>(ctx);
    const float dx = px - c.cx, dy = py - c.cy;
    return sqrtf(dx * dx + dy * dy) - c.r;
}

struct Ring
{
    float cx, cy, r, half;
};

float sdf_ring(float px, float py, const void *ctx)
{
    const Ring &c = *static_cast<const Ring *>(ctx);
    const float dx = px - c.cx, dy = py - c.cy;
    return fabsf(sqrtf(dx * dx + dy * dy) - c.r) - c.half;
}

struct Segment
{
    float x0, y0, x1, y1, half;
};

float sdf_segment(float px, float py, const void *ctx)
{
    const Segment &s = *static_cast<const Segment *>(ctx);
    const float vx = s.x1 - s.x0, vy = s.y1 - s.y0;
    const float wx = px - s.x0, wy = py - s.y0;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0 ? (wx * vx + wy * vy) / len2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy) - s.half;
}

} // namespace

void Canvas::clear(uint32_t color)
{
    fill_rect(0, 0, w_, h_, color);
}

void Canvas::copy_from(const uint32_t *src, int src_pitch)
{
    for (int y = 0; y < h_; ++y)
        memcpy(px_ + (long)y * pitch_, src + (long)y * src_pitch, (size_t)w_ * 4);
}

void Canvas::fill_rect(int x, int y, int w, int h, uint32_t color)
{
    const int x0 = clampi(x, 0, w_), x1 = clampi(x + w, 0, w_);
    const int y0 = clampi(y, 0, h_), y1 = clampi(y + h, 0, h_);
    const uint32_t c = 0xFF000000u | color;
    for (int yy = y0; yy < y1; ++yy)
    {
        uint32_t *row = px_ + (long)yy * pitch_;
        for (int xx = x0; xx < x1; ++xx)
            row[xx] = c;
    }
}

void Canvas::blend_rect(int x, int y, int w, int h, uint32_t color, int alpha)
{
    if (alpha >= 255)
    {
        fill_rect(x, y, w, h, color);
        return;
    }
    const int x0 = clampi(x, 0, w_), x1 = clampi(x + w, 0, w_);
    const int y0 = clampi(y, 0, h_), y1 = clampi(y + h, 0, h_);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            blend_px(xx, yy, color, alpha);
}

void Canvas::vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int i = 0; i < h; ++i)
    {
        const int t = h > 1 ? i * 255 / (h - 1) : 0;
        uint32_t c = 0;
        for (int sh = 0; sh <= 16; sh += 8)
        {
            const int a = (int)((top >> sh) & 0xFF), b = (int)((bottom >> sh) & 0xFF);
            c |= (uint32_t)((a * (255 - t) + b * t) / 255) << sh;
        }
        fill_rect(x, y + i, w, 1, c);
    }
}

void Canvas::coverage_shape(float x0, float y0, float x1, float y1, uint32_t color, int alpha,
                            float (*sdf)(float, float, const void *), const void *ctx)
{
    const int ix0 = clampi((int)floorf(x0) - 1, 0, w_);
    const int ix1 = clampi((int)ceilf(x1) + 1, 0, w_);
    const int iy0 = clampi((int)floorf(y0) - 1, 0, h_);
    const int iy1 = clampi((int)ceilf(y1) + 1, 0, h_);
    for (int y = iy0; y < iy1; ++y)
    {
        for (int x = ix0; x < ix1; ++x)
        {
            const float d = sdf((float)x + 0.5f, (float)y + 0.5f, ctx);
            if (d >= 0.5f)
                continue;
            float cov = 0.5f - d;
            if (cov > 1.0f)
                cov = 1.0f;
            blend_px(x, y, color, (int)(cov * (float)alpha + 0.5f));
        }
    }
}

void Canvas::round_rect(float x, float y, float w, float h, float r, uint32_t color, int alpha)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w * 0.5f)
        r = w * 0.5f;
    if (r > h * 0.5f)
        r = h * 0.5f;
    if (r < 0.5f)
    {
        blend_rect((int)x, (int)y, (int)w, (int)h, color, alpha);
        return;
    }
    // Interior rows without corners: fast fill; corner bands: coverage.
    const int ry0 = (int)ceilf(y + r), ry1 = (int)floorf(y + h - r);
    if (ry1 > ry0)
    {
        // Straight vertical edges still need AA columns at fractional x.
        const int cx0 = (int)ceilf(x), cx1 = (int)floorf(x + w);
        if (alpha >= 255)
            fill_rect(cx0, ry0, cx1 - cx0, ry1 - ry0, color);
        else
            blend_rect(cx0, ry0, cx1 - cx0, ry1 - ry0, color, alpha);
        const float fl = (float)cx0 - x, fr = (x + w) - (float)cx1;
        for (int yy = ry0; yy < ry1; ++yy)
        {
            if (fl > 0.01f)
                blend_px(cx0 - 1, yy, color, (int)(fl * alpha));
            if (fr > 0.01f)
                blend_px(cx1, yy, color, (int)(fr * alpha));
        }
    }
    RoundRect rr{x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, r};
    coverage_shape(x, y, x + w, (float)ry0 - 1.0f, color, alpha, sdf_round_rect, &rr);
    coverage_shape(x, (float)ry1 + 1.0f, x + w, y + h, color, alpha, sdf_round_rect, &rr);
}

void Canvas::round_rect_outline(float x, float y, float w, float h, float r, float thickness,
                                uint32_t color, int alpha)
{
    const float half = thickness * 0.5f;
    RoundRectRing s{{x + w * 0.5f, y + h * 0.5f, w * 0.5f - half, h * 0.5f - half,
                     r - half > 0 ? r - half : 0},
                    half};
    const int ix0 = clampi((int)floorf(x) - 1, 0, w_), ix1 = clampi((int)ceilf(x + w) + 1, 0, w_);
    const int iy0 = clampi((int)floorf(y) - 1, 0, h_), iy1 = clampi((int)ceilf(y + h) + 1, 0, h_);
    const float band = (r > thickness ? r : thickness) + 2.0f;
    const int left_end = (int)ceilf(x + thickness) + 2, right_begin = (int)floorf(x + w - thickness) - 2;
    for (int yy = iy0; yy < iy1; ++yy)
    {
        const bool middle = (float)yy > y + band && (float)yy < y + h - band;
        for (int xx = ix0; xx < ix1; ++xx)
        {
            if (middle && xx >= left_end && xx < right_begin)
                xx = right_begin;
            const float d = sdf_round_rect_ring((float)xx + 0.5f, (float)yy + 0.5f, &s);
            if (d >= 0.5f)
                continue;
            float cov = 0.5f - d;
            if (cov > 1.0f)
                cov = 1.0f;
            blend_px(xx, yy, color, (int)(cov * (float)alpha + 0.5f));
        }
    }
}

void Canvas::circle(float cx, float cy, float r, uint32_t color, int alpha)
{
    Circle c{cx, cy, r};
    coverage_shape(cx - r, cy - r, cx + r, cy + r, color, alpha, sdf_circle, &c);
}

void Canvas::ring(float cx, float cy, float r, float thickness, uint32_t color, int alpha)
{
    Ring c{cx, cy, r, thickness * 0.5f};
    const float e = r + thickness;
    coverage_shape(cx - e, cy - e, cx + e, cy + e, color, alpha, sdf_ring, &c);
}

void Canvas::line(float x0, float y0, float x1, float y1, float thickness, uint32_t color,
                  int alpha)
{
    Segment s{x0, y0, x1, y1, thickness * 0.5f};
    const float e = thickness;
    coverage_shape((x0 < x1 ? x0 : x1) - e, (y0 < y1 ? y0 : y1) - e, (x0 > x1 ? x0 : x1) + e,
                   (y0 > y1 ? y0 : y1) + e, color, alpha, sdf_segment, &s);
}

int Canvas::measure(const Font &font, const char *str)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)str; *p; ++p)
    {
        int ch = *p;
        if (ch < font.first || ch > font.last)
            ch = '?';
        w += font.glyphs[ch - font.first].advance;
    }
    return w;
}

int Canvas::text(const Font &font, int x, int top, const char *str, uint32_t color, int alpha)
{
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)str; *p; ++p)
    {
        int ch = *p;
        if (ch < font.first || ch > font.last)
            ch = '?';
        const Glyph &g = font.glyphs[ch - font.first];
        const uint8_t *src = font.pixels + g.offset;
        const int gx = pen + g.left, gy = top + g.top;
        for (int yy = 0; yy < g.h; ++yy)
        {
            const int py = gy + yy;
            if ((unsigned)py >= (unsigned)h_)
                continue;
            for (int xx = 0; xx < g.w; ++xx)
            {
                const int a = src[yy * g.w + xx];
                if (a)
                    blend_px(gx + xx, py, color, a * alpha / 255);
            }
        }
        pen += g.advance;
    }
    return pen - x;
}

int Canvas::text_centered(const Font &font, int cx, int top, const char *str, uint32_t color,
                          int alpha)
{
    return text(font, cx - measure(font, str) / 2, top, str, color, alpha);
}

int Canvas::text_right(const Font &font, int right, int top, const char *str, uint32_t color,
                       int alpha)
{
    return text(font, right - measure(font, str), top, str, color, alpha);
}

void draw_face_button(Canvas &c, FaceButton b, float cx, float cy, float size)
{
    const float r = size * 0.5f;
    c.circle(cx, cy, r, 0x1a2a47, 255);
    c.ring(cx, cy, r - 1.0f, 2.0f, 0x2a3c5e, 255);
    const float s = size * 0.24f;
    const float t = size > 30 ? 3.0f : 2.4f;
    switch (b)
    {
        case FaceButton::cross:
            c.line(cx - s, cy - s, cx + s, cy + s, t, 0x7fb2ff);
            c.line(cx - s, cy + s, cx + s, cy - s, t, 0x7fb2ff);
            break;
        case FaceButton::circle:
            c.ring(cx, cy, s * 1.05f, t, 0xff6b81);
            break;
        case FaceButton::square:
            c.round_rect_outline(cx - s, cy - s, s * 2, s * 2, 1.5f, t, 0xf28fd1);
            break;
        case FaceButton::triangle:
        {
            const float h = s * 1.15f;
            const float ax = cx, ay = cy - h, bx = cx - h * 1.0f, by = cy + h * 0.65f,
                        dx = cx + h * 1.0f, dy = cy + h * 0.65f;
            c.line(ax, ay, bx, by, t, 0x3ddcb4);
            c.line(bx, by, dx, dy, t, 0x3ddcb4);
            c.line(dx, dy, ax, ay, t, 0x3ddcb4);
            break;
        }
    }
}

} // namespace vd4
