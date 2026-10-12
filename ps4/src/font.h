// VITA4 - bitmap font types (atlases are generated into font_data.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

typedef struct
{
    uint32_t offset;  // into the face's pixel array
    uint16_t w, h;    // bitmap size
    int16_t left;     // bitmap x relative to the pen
    int16_t top;      // bitmap y relative to the line top
    uint16_t advance; // pen advance in pixels
} Glyph;

typedef struct
{
    const Glyph *glyphs;
    const uint8_t *pixels;
    int ascent; // baseline distance from the line top
    int height; // line height
    int first, last;
} Font;
