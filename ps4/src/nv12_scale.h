// VITA4 - NV12 -> 32-bit xRGB conversion with nearest-neighbour scaling.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Converts the Vita's NV12 frames straight into the PS4 framebuffer. The
// output is written strictly front to back (never read), which suits the
// write-combined video memory. Rows can be split across threads: every call
// renders [row_begin, row_end) independently. Pure C, host-tested.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum VdScaleMode
{
    VD_SCALE_SHARP = 0, // whole-number zoom (960x544 -> 2x, trims 4 lines top/bottom)
    VD_SCALE_FIT = 1,   // largest zoom that shows the whole picture
    VD_SCALE_ORIGINAL = 2,
    VD_SCALE_COUNT
};

#define VD_SCALE_MAX_DST 4096

typedef struct
{
    int src_w, src_h;
    int dst_w, dst_h;
    int mode;
    int16_t xmap[VD_SCALE_MAX_DST]; // source column per output column, -1 = border
    int16_t ymap[VD_SCALE_MAX_DST]; // source row per output row, -1 = border
} VdScaleMap;

void vd_yuv_tables_init(void);

// Returns 0 on success, -1 on bad sizes.
int vd_scale_map_build(VdScaleMap *map, int src_w, int src_h, int dst_w, int dst_h, int mode);

// Render output rows [row_begin, row_end) of the scaled frame into `dst`
// (pitch in pixels). `scratch` must hold src_w uint32 values (per thread).
void vd_nv12_render_rows(const uint8_t *nv12, const VdScaleMap *map, uint32_t *dst, int pitch,
                         int row_begin, int row_end, uint32_t *scratch);

// Single pixel reference conversion (BT.601 limited range), for tests.
uint32_t vd_nv12_pixel_ref(const uint8_t *nv12, int w, int h, int x, int y);

#define VD_BLACK 0xFF000000u

#ifdef __cplusplus
}
#endif
