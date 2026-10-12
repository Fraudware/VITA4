// VITA4 - "Smooth" upscale. See smooth.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "smooth.h"

#include <emmintrin.h>
#include <string.h>

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---- sharpen ---------------------------------------------------------------------

static inline uint32_t sharpen_px(uint32_t c, uint32_t u, uint32_t d, uint32_t l, uint32_t r, int k)
{
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8)
    {
        const int cc = (int)((c >> s) & 0xFF);
        const int sum = (int)((u >> s) & 0xFF) + (int)((d >> s) & 0xFF) + (int)((l >> s) & 0xFF) +
                        (int)((r >> s) & 0xFF);
        int v = cc + (((4 * cc - sum) * k + 128) >> 8);
        out |= (uint32_t)clampi(v, 0, 255) << s;
    }
    return out;
}

void vd_smooth_sharpen_rows(const uint32_t *src, uint32_t *dst, int w, int h, int k, int begin,
                            int end)
{
    k = clampi(k, 0, 31); // |4c - sum| <= 1020: 1020 * 31 + 128 still fits 16 bits
    begin = clampi(begin, 0, h);
    end = clampi(end, 0, h);
    for (int y = begin; y < end; ++y)
    {
        const uint32_t *c = src + (size_t)y * w;
        const uint32_t *up = src + (size_t)(y > 0 ? y - 1 : 0) * w;
        const uint32_t *dn = src + (size_t)(y < h - 1 ? y + 1 : h - 1) * w;
        uint32_t *o = dst + (size_t)y * w;
        if (k == 0)
        {
            memcpy(o, c, (size_t)w * 4);
            continue;
        }
        o[0] = sharpen_px(c[0], up[0], dn[0], c[0], c[w > 1 ? 1 : 0], k);
        int x = 1;
        const __m128i zero = _mm_setzero_si128();
        const __m128i kk = _mm_set1_epi16((short)k);
        const __m128i half = _mm_set1_epi16(128);
        const __m128i alpha = _mm_set1_epi32((int)0xFF000000u);
        for (; x + 4 <= w - 1; x += 4) // 4 pixels = 16 channel bytes
        {
            const __m128i vc = _mm_loadu_si128((const __m128i *)(c + x));
            const __m128i vu = _mm_loadu_si128((const __m128i *)(up + x));
            const __m128i vd = _mm_loadu_si128((const __m128i *)(dn + x));
            const __m128i vl = _mm_loadu_si128((const __m128i *)(c + x - 1));
            const __m128i vr = _mm_loadu_si128((const __m128i *)(c + x + 1));
            __m128i res[2];
            for (int part = 0; part < 2; ++part)
            {
#define WIDEN(v) (part ? _mm_unpackhi_epi8((v), zero) : _mm_unpacklo_epi8((v), zero))
                const __m128i c16 = WIDEN(vc);
                const __m128i sum = _mm_add_epi16(_mm_add_epi16(WIDEN(vu), WIDEN(vd)),
                                                  _mm_add_epi16(WIDEN(vl), WIDEN(vr)));
#undef WIDEN
                const __m128i diff = _mm_sub_epi16(_mm_slli_epi16(c16, 2), sum); // 4c - sum
                // (diff * k + 128) >> 8, arithmetic shift
                const __m128i prod = _mm_mullo_epi16(diff, kk);
                const __m128i adj = _mm_srai_epi16(_mm_add_epi16(prod, half), 8);
                res[part] = _mm_add_epi16(c16, adj);
            }
            const __m128i packed = _mm_or_si128(_mm_packus_epi16(res[0], res[1]), alpha);
            _mm_storeu_si128((__m128i *)(o + x), packed);
        }
        for (; x < w; ++x)
            o[x] = sharpen_px(c[x], up[x], dn[x], c[x - 1], c[x < w - 1 ? x + 1 : x], k);
    }
}

// ---- 2x bilinear --------------------------------------------------------------------
// At 2x each output pixel is 1/4 texel from its nearest source pixel `n` and
// 3/4 from the next one `f` on that axis: (3n + f) / 4 = avg(avg(n, f), n).
// The inner average rounds up (pavgb), the outer one rounds down, so the two
// roundings cancel on average instead of brightening the picture.

static inline __m128i avg_floor(__m128i a, __m128i b)
{
    // pavgb rounds up: subtract the carried bit where a + b is odd.
    const __m128i one = _mm_set1_epi8(1);
    return _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), one));
}

static inline __m128i blend31(__m128i n, __m128i f)
{
    return avg_floor(_mm_avg_epu8(n, f), n);
}

static inline uint32_t blend31_px(uint32_t n, uint32_t f)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
    {
        const unsigned a = (n >> s) & 0xFF, b = (f >> s) & 0xFF;
        const unsigned inner = (a + b + 1) >> 1;
        out |= ((inner + a) >> 1) << s;
    }
    return out;
}

void vd_smooth_render_rows(const uint32_t *src, int w, int h, uint32_t *dst, int pitch,
                           int row_offset, int dst_h, int begin, int end, uint32_t *scratch)
{
    begin = clampi(begin, 0, dst_h);
    end = clampi(end, 0, dst_h);
    const int oh = h * 2;
    uint32_t *v = scratch + 1; // v[-1] .. v[w]
    for (int y = begin; y < end; ++y)
    {
        const int sy = y + row_offset;
        if (sy < 0 || sy >= oh)
            continue;
        const int near = sy >> 1;
        const int far = clampi((sy & 1) ? near + 1 : near - 1, 0, h - 1);
        const uint32_t *rn = src + (size_t)near * w, *rf = src + (size_t)far * w;

        // Vertical blend into the padded scratch row.
        int x = 0;
        for (; x + 4 <= w; x += 4)
        {
            const __m128i a = _mm_loadu_si128((const __m128i *)(rn + x));
            const __m128i b = _mm_loadu_si128((const __m128i *)(rf + x));
            _mm_storeu_si128((__m128i *)(v + x), blend31(a, b));
        }
        for (; x < w; ++x)
            v[x] = blend31_px(rn[x], rf[x]);
        v[-1] = v[0];
        v[w] = v[w - 1];

        // Horizontal: output 2i from (v[i], v[i-1]), 2i+1 from (v[i], v[i+1]).
        uint32_t *o = dst + (size_t)y * pitch;
        int i = 0;
        for (; i + 4 <= w; i += 4)
        {
            const __m128i a = _mm_loadu_si128((const __m128i *)(v + i));
            const __m128i l = _mm_loadu_si128((const __m128i *)(v + i - 1));
            const __m128i r = _mm_loadu_si128((const __m128i *)(v + i + 1));
            const __m128i ev = blend31(a, l);
            const __m128i od = blend31(a, r);
            _mm_storeu_si128((__m128i *)(o + 2 * i), _mm_unpacklo_epi32(ev, od));
            _mm_storeu_si128((__m128i *)(o + 2 * i + 4), _mm_unpackhi_epi32(ev, od));
        }
        for (; i < w; ++i)
        {
            o[2 * i] = blend31_px(v[i], v[i - 1]);
            o[2 * i + 1] = blend31_px(v[i], v[i + 1]);
        }
    }
}
