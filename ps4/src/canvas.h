// VITA4 - software 2D drawing into a 32-bit xRGB pixel buffer.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything the PS4 UI needs (filled/rounded/translucent rects, anti-aliased
// circles, rings and lines, bitmap text) without a GPU library. Portable: the
// host test build renders the same screens to PNG for review.
#pragma once
#include <stdint.h>

#include "font.h"

namespace vd4
{

// 0xRRGGBB
inline uint32_t rgb(uint32_t c)
{
    return 0xFF000000u | (c & 0xFFFFFFu);
}

class Canvas
{
  public:
    Canvas() = default;
    Canvas(uint32_t *pixels, int width, int height, int pitch)
        : px_(pixels), w_(width), h_(height), pitch_(pitch)
    {
    }

    uint32_t *pixels() const
    {
        return px_;
    }
    int width() const
    {
        return w_;
    }
    int height() const
    {
        return h_;
    }
    int pitch() const
    {
        return pitch_;
    }

    void clear(uint32_t color);
    void copy_from(const uint32_t *src, int src_pitch);

    // Opaque fill / alpha blend (alpha 0..255). Colours are 0xRRGGBB.
    void fill_rect(int x, int y, int w, int h, uint32_t color);
    void blend_rect(int x, int y, int w, int h, uint32_t color, int alpha);
    void round_rect(float x, float y, float w, float h, float r, uint32_t color, int alpha = 255);
    void round_rect_outline(float x, float y, float w, float h, float r, float thickness,
                            uint32_t color, int alpha = 255);
    void circle(float cx, float cy, float r, uint32_t color, int alpha = 255);
    void ring(float cx, float cy, float r, float thickness, uint32_t color, int alpha = 255);
    void line(float x0, float y0, float x1, float y1, float thickness, uint32_t color,
              int alpha = 255);
    void vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

    // Text: (x, top) is the top-left of the line box. Returns the advance.
    int text(const Font &font, int x, int top, const char *str, uint32_t color, int alpha = 255);
    // Text with its baseline-centred anchor variants.
    int text_centered(const Font &font, int cx, int top, const char *str, uint32_t color,
                      int alpha = 255);
    int text_right(const Font &font, int right, int top, const char *str, uint32_t color,
                   int alpha = 255);
    static int measure(const Font &font, const char *str);

    // Blend a single pixel (coverage 0..255 already multiplied with alpha).
    inline void blend_px(int x, int y, uint32_t color, int a)
    {
        if ((unsigned)x >= (unsigned)w_ || (unsigned)y >= (unsigned)h_ || a <= 0)
            return;
        uint32_t *p = px_ + (long)y * pitch_ + x;
        if (a >= 255)
        {
            *p = 0xFF000000u | color;
            return;
        }
        const uint32_t d = *p;
        const uint32_t ia = 255 - (uint32_t)a;
        const uint32_t rb = ((color & 0xFF00FFu) * (uint32_t)a + (d & 0xFF00FFu) * ia + 0x800080u);
        const uint32_t g = ((color & 0x00FF00u) * (uint32_t)a + (d & 0x00FF00u) * ia + 0x008000u);
        *p = 0xFF000000u | (((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu) |
             (((g + ((g >> 8) & 0x00FF00u)) >> 8) & 0x00FF00u);
    }

  private:
    void coverage_shape(float x0, float y0, float x1, float y1, uint32_t color, int alpha,
                        float (*sdf)(float, float, const void *), const void *ctx);

    uint32_t *px_ = nullptr;
    int w_ = 0, h_ = 0, pitch_ = 0;
};

// PlayStation face-button glyphs, drawn as shapes (cross, circle, square,
// triangle) inside a soft badge. `size` is the badge diameter.
enum class FaceButton
{
    cross,
    circle,
    square,
    triangle
};
void draw_face_button(Canvas &c, FaceButton b, float cx, float cy, float size);

} // namespace vd4
