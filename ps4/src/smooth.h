// VITA4 - "Smooth" upscale: a light sharpen + 2x bilinear in 8-bit SIMD.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// FSR on the PS4's CPU is too slow for a steady 30 fps, so this mode keeps to
// whole-number byte maths the CPU does 16 lanes at a time:
//   1. the Vita picture as xRGB at its own size (nv12_scale.c, 1:1),
//   2. vd_smooth_sharpen_rows: unsharp mask on the 4-neighbour cross at that
//      size (a quarter of the output pixels), strength k/64,
//   3. vd_smooth_render_rows: 2x bilinear (3:1 weights, as at 2x every output
//      pixel sits a quarter texel from its nearest source pixel), built from
//      byte averages, straight into the screen buffer.
// Rows of each step can run on many threads. Pure C + SSE2, host-tested.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sharpen rows [begin, end) of a w x h xRGB picture from src into dst
// (different buffers, both pitch w): c + k/64 * (c - average of the 4
// neighbours). k = 0 copies; k is clamped to 0..31 (16-bit headroom).
void vd_smooth_sharpen_rows(const uint32_t *src, uint32_t *dst, int w, int h, int k, int begin,
                            int end);

// Output rows [begin, end) of the 2x picture: output row y is 2x row
// y + row_offset (the crop: 544 -> 1088 rows shown as 1080 with row_offset 4),
// written into dst (pitch in pixels). `scratch` holds w + 2 pixels per thread.
void vd_smooth_render_rows(const uint32_t *src, int w, int h, uint32_t *dst, int pitch,
                           int row_offset, int dst_h, int begin, int end, uint32_t *scratch);

#ifdef __cplusplus
}
#endif
