// VITA4 - NV12 conversion and scaling. See nv12_scale.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nv12_scale.h"

#include <stddef.h>

// BT.601 limited range, the same coefficients as the PS5 app's nv12.cpp:
//   R = (298*(Y-16) + 409*(V-128) + 128) >> 8
//   G = (298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8
//   B = (298*(Y-16) + 516*(U-128) + 128) >> 8
static int g_y[256], g_rv[256], g_gu[256], g_gv[256], g_bu[256];
static uint8_t g_clamp_storage[1024 + 256];
static const uint8_t *g_clamp = g_clamp_storage + 512; // index range -512..767
static int g_ready;

void vd_yuv_tables_init(void)
{
    if (g_ready)
        return;
    for (int i = 0; i < 256; ++i)
    {
        g_y[i] = 298 * (i - 16) + 128;
        g_rv[i] = 409 * (i - 128);
        g_gu[i] = -100 * (i - 128);
        g_gv[i] = -208 * (i - 128);
        g_bu[i] = 516 * (i - 128);
    }
    for (int i = -512; i < 768; ++i)
        g_clamp_storage[i + 512] = (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
    g_ready = 1;
}

static inline uint32_t yuv_to_xrgb(int y, int u, int v)
{
    const int c = g_y[y];
    const int r = (c + g_rv[v]) >> 8;
    const int g = (c + g_gu[u] + g_gv[v]) >> 8;
    const int b = (c + g_bu[u]) >> 8;
    return 0xFF000000u | ((uint32_t)g_clamp[r] << 16) | ((uint32_t)g_clamp[g] << 8) |
           (uint32_t)g_clamp[b];
}

uint32_t vd_nv12_pixel_ref(const uint8_t *nv12, int w, int h, int x, int y)
{
    const uint8_t *yp = nv12;
    const uint8_t *uvp = nv12 + (size_t)w * h;
    const int yv = (int)yp[y * w + x] - 16;
    const int idx = (y / 2) * w + (x & ~1);
    const int u = (int)uvp[idx] - 128;
    const int v = (int)uvp[idx + 1] - 128;
    const int c = 298 * yv;
    int r = (c + 409 * v + 128) >> 8;
    int g = (c - 100 * u - 208 * v + 128) >> 8;
    int b = (c + 516 * u + 128) >> 8;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int vd_scale_map_build(VdScaleMap *map, int src_w, int src_h, int dst_w, int dst_h, int mode)
{
    if (!map || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        dst_w > VD_SCALE_MAX_DST || dst_h > VD_SCALE_MAX_DST || src_w > 32767 || src_h > 32767)
        return -1;
    map->src_w = src_w;
    map->src_h = src_h;
    map->dst_w = dst_w;
    map->dst_h = dst_h;
    map->mode = mode;

    // Scale as a rational num/den so the maps are exact integers.
    long num, den;
    if (mode == VD_SCALE_ORIGINAL)
    {
        num = 1;
        den = 1;
    }
    else
    {
        // Fit: min(dst_w/src_w, dst_h/src_h).
        if ((long)dst_w * src_h <= (long)dst_h * src_w)
        {
            num = dst_w;
            den = src_w;
        }
        else
        {
            num = dst_h;
            den = src_h;
        }
        if (mode == VD_SCALE_SHARP)
        {
            // Whole-number zoom; allow a few percent of overscan (960x544 is
            // 1.985x of 1080 lines, so 2x trims only 4 lines top and bottom).
            long k = (num * 100 + den * 4) / (den * 100); // floor(scale + 0.04)
            if (k < 1)
                k = 1;
            num = k;
            den = 1;
        }
    }

    const long out_w = (long)src_w * num / den;
    const long out_h = (long)src_h * num / den;
    const long ox = ((long)dst_w - out_w) / 2; // may be negative (crop)
    const long oy = ((long)dst_h - out_h) / 2;

    for (int x = 0; x < dst_w; ++x)
    {
        long rel = x - ox;
        long sx = rel >= 0 ? rel * den / num : -1;
        map->xmap[x] = (int16_t)((rel < 0 || rel >= out_w || sx >= src_w) ? -1 : sx);
    }
    for (int y = 0; y < dst_h; ++y)
    {
        long rel = y - oy;
        long sy = rel >= 0 ? rel * den / num : -1;
        map->ymap[y] = (int16_t)((rel < 0 || rel >= out_h || sy >= src_h) ? -1 : sy);
    }
    return 0;
}

static void convert_source_row(const uint8_t *nv12, int w, int h, int sy, uint32_t *out)
{
    const uint8_t *yrow = nv12 + (size_t)sy * w;
    const uint8_t *uvrow = nv12 + (size_t)w * h + (size_t)(sy >> 1) * w;
    int x = 0;
    for (; x + 1 < w; x += 2)
    {
        const int u = uvrow[x];
        const int v = uvrow[x + 1];
        out[x] = yuv_to_xrgb(yrow[x], u, v);
        out[x + 1] = yuv_to_xrgb(yrow[x + 1], u, v);
    }
    if (x < w) // odd width: last pixel shares the previous pair's chroma
        out[x] = yuv_to_xrgb(yrow[x], uvrow[x - 1], uvrow[x]);
}

void vd_nv12_render_rows(const uint8_t *nv12, const VdScaleMap *map, uint32_t *dst, int pitch,
                         int row_begin, int row_end, uint32_t *scratch)
{
    if (!g_ready)
        vd_yuv_tables_init();
    if (row_begin < 0)
        row_begin = 0;
    if (row_end > map->dst_h)
        row_end = map->dst_h;
    const int w = map->src_w, h = map->src_h, dw = map->dst_w;

    // First and last visible output columns, so borders are plain fills.
    int x0 = 0, x1 = dw;
    while (x0 < dw && map->xmap[x0] < 0)
        ++x0;
    while (x1 > x0 && map->xmap[x1 - 1] < 0)
        --x1;

    int cached = -1;
    for (int y = row_begin; y < row_end; ++y)
    {
        uint32_t *out = dst + (size_t)y * pitch;
        const int sy = map->ymap[y];
        if (sy < 0 || nv12 == NULL)
        {
            for (int x = 0; x < dw; ++x)
                out[x] = VD_BLACK;
            continue;
        }
        if (sy != cached)
        {
            convert_source_row(nv12, w, h, sy, scratch);
            cached = sy;
        }
        int x = 0;
        for (; x < x0; ++x)
            out[x] = VD_BLACK;
        for (; x < x1; ++x)
            out[x] = scratch[map->xmap[x]];
        for (; x < dw; ++x)
            out[x] = VD_BLACK;
    }
}
