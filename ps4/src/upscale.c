// VITA4 - CPU port of VITA5's Sharp / FSR upscale chain. See upscale.h.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EASU (FsrEasuF) and RCAS (FsrRcasF) are AMD FidelityFX FSR 1.0
// (ffx_fsr1.h, MIT, licenses/FSR1-MIT.txt), following VITA5's GLSL port in
// app/src/vd/upscale_shaders.hpp.
//
// Luma-only (1.11): the Vita's NV12 video keeps colour at half resolution in
// each direction, so all the picture detail is in luma. EASU and RCAS run on
// luma (one channel instead of three); colour is scaled smoothly (bilinear)
// from the chroma plane and added back at the end. On grey pictures this is
// exactly VITA5's RGB chain; on colour it differs only in colour edges that
// the 4:2:0 video never carried. About 3x less work for the PS4's CPU.
//
// Planes carry PAD replicated columns on each side, so the column clamp of
// the shaders' texelFetch(clamp(p)) costs nothing; rows are clamped once per
// output row.
#include "upscale.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PAD 2

struct VdUpscaler
{
    int w, h;     // source (luma)
    int ow, oh;   // output (2x)
    int stride;   // luma plane row pitch (w + 2 * PAD)
    int cw, ch;   // chroma plane size (w/2 x h/2)
    int cstride;  // chroma plane row pitch (cw + 2)
    float *mem;
    float *l;             // luma, 0..1 (col 0 at offset PAD)
    float *dx, *dy, *len; // per-texel EASU analysis (FsrEasuSetF terms)
    float *cr, *cg, *cb;  // chroma's additive share of R, G, B (col 0 at offset 1)
    float rcas_con;   // RCAS strength (FsrRcasCon: 1 = full); 0 = no sharpening
    int rcas_denoise; // FSR_RCAS_DENOISE (FSR 2 turns it on)
    uint8_t *flatmem;
    uint8_t *flat; // per quad (fx, fy) of EASU's 4 nearest taps: 1 if they are within
                   // kFlat of each other (row fy at (fy + 1) * stride, col 0 at PAD)
};

// Quads whose 4 nearest luma texels lie within this range skip the 12-tap
// kernel: EASU's own dering clamp keeps its result inside that range, so the
// bilinear result differs by less than one 8-bit level.
#define kFlat (1.0f / 255.0f)

const char *vd_upscale_label(int mode)
{
    switch (mode)
    {
        case VD_UPSCALE_SHARP:
            return "Sharp";
        case VD_UPSCALE_FSR:
            return "FSR";
        case VD_UPSCALE_SMOOTH:
            return "Smooth";
        case VD_UPSCALE_TV:
            return "TV scaler";
        default:
            return "Off";
    }
}

int vd_upscale_next(int mode, int dir)
{
    static const int order[] = {VD_UPSCALE_OFF, VD_UPSCALE_SMOOTH, VD_UPSCALE_TV, VD_UPSCALE_SHARP,
                                VD_UPSCALE_FSR};
    const int n = (int)(sizeof(order) / sizeof(order[0]));
    int at = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == mode)
            at = i;
    return order[((at + dir) % n + n) % n];
}

int vd_upscale_uses_worker(int mode)
{
    return mode == VD_UPSCALE_SHARP || mode == VD_UPSCALE_FSR;
}

VdUpscaler *vd_upscaler_create(int src_w, int src_h)
{
    if (src_w < 4 || src_h < 4 || src_w > 4096 || src_h > 4096 || (src_w & 1) || (src_h & 1))
        return NULL;
    VdUpscaler *u = (VdUpscaler *)calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->w = src_w;
    u->h = src_h;
    u->ow = src_w * 2;
    u->oh = src_h * 2;
    u->stride = src_w + 2 * PAD;
    u->cw = src_w / 2;
    u->ch = src_h / 2;
    u->cstride = u->cw + 2;
    const size_t ln = (size_t)u->stride * src_h, cn = (size_t)u->cstride * u->ch;
    const size_t total = ln * 4 + cn * 3;
    u->mem = (float *)malloc(total * sizeof(float));
    if (!u->mem)
    {
        free(u);
        return NULL;
    }
    memset(u->mem, 0, total * sizeof(float)); // commits the pages now
    float *p = u->mem;
    u->l = p + PAD;
    u->dx = p + ln + PAD;
    u->dy = p + 2 * ln + PAD;
    u->len = p + 3 * ln + PAD;
    p += 4 * ln;
    u->cr = p + 1;
    u->cg = p + cn + 1;
    u->cb = p + 2 * cn + 1;
    const size_t fn = (size_t)u->stride * (src_h + 1);
    u->flatmem = (uint8_t *)malloc(fn);
    if (!u->flatmem)
    {
        free(u->mem);
        free(u);
        return NULL;
    }
    memset(u->flatmem, 0, fn);
    u->flat = u->flatmem + PAD;
    u->rcas_con = 1.0f; // VITA5: FsrRcasCon(0.0), full sharpness
    u->rcas_denoise = 0;
    return u;
}

void vd_upscaler_destroy(VdUpscaler *u)
{
    if (!u)
        return;
    free(u->mem);
    free(u->flatmem);
    free(u);
}

void vd_upscaler_set_sharpness(VdUpscaler *u, float sharpness, int denoise)
{
    if (sharpness <= 0.0f)
        u->rcas_con = 0.0f; // sharpening off
    else
    {
        // FSR 2 (ffx_fsr2.cpp): stops = 2 - 2 * sharpness, FsrRcasCon -> 2^-stops.
        const float s = sharpness > 1.0f ? 1.0f : sharpness;
        u->rcas_con = exp2f(-(2.0f - 2.0f * s));
    }
    u->rcas_denoise = denoise ? 1 : 0;
}

int vd_upscaler_src_w(const VdUpscaler *u)
{
    return u->w;
}
int vd_upscaler_src_h(const VdUpscaler *u)
{
    return u->h;
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float minf(float a, float b)
{
    return a < b ? a : b;
}
static inline float maxf(float a, float b)
{
    return a > b ? a : b;
}
static inline float absf(float a)
{
    return a < 0 ? -a : a;
}

static inline void pad_row(float *row, int w, int pad)
{
    for (int i = 1; i <= pad; ++i)
    {
        row[-i] = row[0];
        row[w - 1 + i] = row[w - 1];
    }
}

// ---- convert: NV12 -> luma + chroma terms (VITA5 kYuv math) -------------------
// R = (298 (Y-16) + 409 V + 128) / 256 / 255 and so on, split into a luma part
// L = 298 (Y-16) / 65280 and per-channel chroma parts added back at the end.

#define K (1.0f / 65280.0f)

void vd_upscale_convert(VdUpscaler *u, const uint8_t *nv12, int begin, int end)
{
    begin = clampi(begin, 0, u->h);
    end = clampi(end, 0, u->h);
    const int w = u->w;
    for (int y = begin; y < end; ++y)
    {
        const uint8_t *yrow = nv12 + (size_t)y * w;
        float *l = u->l + (size_t)y * u->stride;
        for (int x = 0; x < w; ++x)
            l[x] = clampf((float)((int)yrow[x] - 16) * (298.0f * K), 0.0f, 1.0f);
        pad_row(l, w, PAD);
        if (y & 1)
            continue; // each chroma row belongs to the even luma row above it
        const int cy = y / 2;
        const uint8_t *uv = nv12 + (size_t)w * u->h + (size_t)cy * w;
        const size_t o = (size_t)cy * u->cstride;
        float *cr = u->cr + o, *cg = u->cg + o, *cb = u->cb + o;
        for (int x = 0; x < u->cw; ++x)
        {
            const float uu = (float)((int)uv[2 * x] - 128), vv = (float)((int)uv[2 * x + 1] - 128);
            cr[x] = (409.0f * vv + 128.0f) * K;
            cg[x] = (-100.0f * uu - 208.0f * vv + 128.0f) * K;
            cb[x] = (516.0f * uu + 128.0f) * K;
        }
        pad_row(cr, u->cw, 1);
        pad_row(cg, u->cw, 1);
        pad_row(cb, u->cw, 1);
    }
}

// ---- analyze: FsrEasuSetF's terms per texel (on luma2 = 2 L) ---------------------
// Direction (right - left, down - up) and the squared, clamped edge lengths of
// the texel at (x, y); EASU blends the four central texels' terms bilinearly,
// exactly as easu_set(dir, len, w, A, B, C, D, E) accumulates them.

void vd_upscale_analyze(VdUpscaler *u, int begin, int end)
{
    begin = clampi(begin, 0, u->h);
    end = clampi(end, 0, u->h);
    const int w = u->w, s = u->stride;
    for (int y = begin; y < end; ++y)
    {
        const float *la = u->l + (size_t)clampi(y - 1, 0, u->h - 1) * s;
        const float *lc = u->l + (size_t)y * s;
        const float *le = u->l + (size_t)clampi(y + 1, 0, u->h - 1) * s;
        float *dx = u->dx + (size_t)y * s, *dy = u->dy + (size_t)y * s;
        float *len = u->len + (size_t)y * s;
        for (int x = 0; x < w; ++x)
        {
            const float lA = 2.0f * la[x], lB = 2.0f * lc[x - 1], lC = 2.0f * lc[x],
                        lD = 2.0f * lc[x + 1], lE = 2.0f * le[x];
            const float dirX = lD - lB;
            float lenX = 1.0f / maxf(maxf(absf(lD - lC), absf(lC - lB)), 1e-6f);
            lenX = clampf(absf(dirX) * lenX, 0.0f, 1.0f);
            const float dirY = lE - lA;
            float lenY = 1.0f / maxf(maxf(absf(lE - lC), absf(lC - lA)), 1e-6f);
            lenY = clampf(absf(dirY) * lenY, 0.0f, 1.0f);
            dx[x] = dirX;
            dy[x] = dirY;
            len[x] = lenX * lenX + lenY * lenY;
        }
        pad_row(dx, w, PAD);
        pad_row(dy, w, PAD);
        pad_row(len, w, PAD);

        // Flatness of the quads EASU sees around source row y (its 'f' row
        // fy = y), and fy = -1 for the band that owns the top row.
        for (int fy = (y == 0 ? -1 : y); fy <= y; ++fy)
        {
            const float *r1 = u->l + (size_t)clampi(fy, 0, u->h - 1) * s;
            const float *r2 = u->l + (size_t)clampi(fy + 1, 0, u->h - 1) * s;
            uint8_t *f = u->flat + (size_t)(fy + 1) * s;
            for (int x = -1; x < w; ++x)
            {
                const float mn = minf(minf(r1[x], r1[x + 1]), minf(r2[x], r2[x + 1]));
                const float mx = maxf(maxf(r1[x], r1[x + 1]), maxf(r2[x], r2[x + 1]));
                f[x] = (uint8_t)(mx - mn < kFlat);
            }
        }
    }
}

// ---- scale: 2x luma by phase ----------------------------------------------------
// Output column ox = 2*fx + 1 sits at phase 0.25 of source texel fx (fx in
// [0, w)); ox = 2*fx + 2 at phase 0.75 (fx in [-1, w - 1)). Rows likewise.

typedef struct
{
    const float *l[4];                   // luma rows fy-1 .. fy+2 (clamped)
    const float *dx[2], *dy[2], *len[2]; // analysis rows fy, fy+1
    const uint8_t *flat;                 // flat-quad flags for row fy
} Rows;

// FsrEasuTapF: windowed-lanczos2 tap, rearranged for fewer multiplies:
//  - the tap offset (OX - ppx, OY - ppy) projected on the rotated, stretched
//    axes is AX[OX] + BX[OY] (and AY + BY), sums of per-pixel terms, so a tap
//    costs two adds instead of four multiplies;
//  - FSR's base window (25/16)(2/5 d2 - 1)^2 - (25/16 - 1) equals
//    ((d2 - 5/2)^2 - 9/4) / 4, and the constant 1/4 cancels in acc / aw.
#define EASU_TAP(VX, VY, V)                                                                   \
    do                                                                                        \
    {                                                                                         \
        const float vx_ = (VX), vy_ = (VY);                                                   \
        const float d2 = minf(vx_ * vx_ + vy_ * vy_, clp);                                    \
        const float t = d2 - 2.5f;                                                            \
        const float s = lob * d2 - 1.0f;                                                      \
        const float wt = (t * t - 2.25f) * (s * s);                                           \
        acc += (V) * wt;                                                                      \
        aw += wt;                                                                             \
    } while (0)

// One phase of one output row: n pixels for source columns fx0 .. fx0+n-1.
static inline __attribute__((always_inline)) void easu_core(const Rows *R, int fx0, int n,
                                                            const float ppx, const float ppy,
                                                            float *restrict out)
{
    const float *l0 = R->l[0] + fx0, *l1 = R->l[1] + fx0, *l2 = R->l[2] + fx0, *l3 = R->l[3] + fx0;
    const float *dx0 = R->dx[0] + fx0, *dx1 = R->dx[1] + fx0;
    const float *dy0 = R->dy[0] + fx0, *dy1 = R->dy[1] + fx0;
    const float *ln0 = R->len[0] + fx0, *ln1 = R->len[1] + fx0;
    const float wS = (1.0f - ppx) * (1.0f - ppy), wT = ppx * (1.0f - ppy);
    const float wU = (1.0f - ppx) * ppy, wV = ppx * ppy;
    for (int i = 0; i < n; ++i)
    {
        //    b c        row 0
        //  e f g h      row 1
        //  i j k l      row 2
        //    n o        row 3
        float dirx = dx0[i] * wS + dx0[i + 1] * wT + dx1[i] * wU + dx1[i + 1] * wV;
        float diry = dy0[i] * wS + dy0[i + 1] * wT + dy1[i] * wU + dy1[i + 1] * wV;
        float len = ln0[i] * wS + ln0[i + 1] * wT + ln1[i] * wU + ln1[i + 1] * wV;

        // Normalize and clean up close to zero.
        const float dirR0 = dirx * dirx + diry * diry;
        const int zro = dirR0 < (1.0f / 32768.0f);
        float dirR = 1.0f / sqrtf(maxf(dirR0, 1e-12f));
        dirR = zro ? 1.0f : dirR;
        dirx = zro ? 1.0f : dirx;
        dirx *= dirR;
        diry *= dirR;

        // Shape the length, stretch the kernel along the gradient.
        len = len * 0.5f;
        len *= len;
        const float stretch = (dirx * dirx + diry * diry) / maxf(absf(dirx), absf(diry));
        const float len2x = 1.0f + (stretch - 1.0f) * len;
        const float len2y = 1.0f + (-0.5f) * len;
        const float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
        const float clp = 1.0f / maxf(lob, 1e-6f);
        const float kxa = dirx * len2x, kxb = diry * len2x;
        const float kya = -diry * len2y, kyb = dirx * len2y;
        // Tap offsets on the kernel axes: X at column OX = AXo + OX * kxa,
        // row OY adds OY * kxb (Y likewise).
        const float ax0 = -(ppx * kxa + ppy * kxb), ay0 = -(ppx * kya + ppy * kyb);
        const float axm = ax0 - kxa, ax1 = ax0 + kxa, ax2 = ax1 + kxa;
        const float aym = ay0 - kya, ay1 = ay0 + kya, ay2 = ay1 + kya;
        const float bx2 = kxb + kxb, by2 = kyb + kyb;

        // Dering: clip against the 4 nearest taps (f g j k).
        const float mn = minf(minf(l1[i], l1[i + 1]), minf(l2[i], l2[i + 1]));
        const float mx = maxf(maxf(l1[i], l1[i + 1]), maxf(l2[i], l2[i + 1]));

        float acc = 0, aw = 0;
        EASU_TAP(ax0 - kxb, ay0 - kyb, l0[i]);     // b  ( 0,-1)
        EASU_TAP(ax1 - kxb, ay1 - kyb, l0[i + 1]); // c  ( 1,-1)
        EASU_TAP(axm + kxb, aym + kyb, l2[i - 1]); // i  (-1, 1)
        EASU_TAP(ax0 + kxb, ay0 + kyb, l2[i]);     // j  ( 0, 1)
        EASU_TAP(ax0, ay0, l1[i]);                 // f  ( 0, 0)
        EASU_TAP(axm, aym, l1[i - 1]);             // e  (-1, 0)
        EASU_TAP(ax1 + kxb, ay1 + kyb, l2[i + 1]); // k  ( 1, 1)
        EASU_TAP(ax2 + kxb, ay2 + kyb, l2[i + 2]); // l  ( 2, 1)
        EASU_TAP(ax2, ay2, l1[i + 2]);             // h  ( 2, 0)
        EASU_TAP(ax1, ay1, l1[i + 1]);             // g  ( 1, 0)
        EASU_TAP(ax1 + bx2, ay1 + by2, l3[i + 1]); // o  ( 1, 2)
        EASU_TAP(ax0 + bx2, ay0 + by2, l3[i]);     // n  ( 0, 2)

        out[i] = minf(mx, maxf(mn, acc * (1.0f / maxf(aw, 4e-6f))));
    }
}

// GL bilinear (clamp to edge) at a fixed phase: texels (fx, fy) (fx+1, fy)
// (fx, fy+1) (fx+1, fy+1) are rows 1-2, columns i, i+1.
static inline __attribute__((always_inline)) void blit_span(const Rows *R, int fx0, int n,
                                                            const float ppx, const float ppy,
                                                            float *restrict out)
{
    const float w00 = (1 - ppx) * (1 - ppy), w10 = ppx * (1 - ppy);
    const float w01 = (1 - ppx) * ppy, w11 = ppx * ppy;
    const float *l1 = R->l[1] + fx0, *l2 = R->l[2] + fx0;
    for (int i = 0; i < n; ++i)
        out[i] = l1[i] * w00 + l1[i + 1] * w10 + l2[i] * w01 + l2[i + 1] * w11;
}

// EASU for a phase of a row, 8 pixels at a time; runs of 8 flat quads take
// the bilinear path instead (within one level of EASU there, see kFlat).
static inline __attribute__((always_inline)) void easu_span(const Rows *R, int fx0, int n,
                                                            const float ppx, const float ppy,
                                                            float *restrict out)
{
    const uint8_t *flat = R->flat + fx0;
    int i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t f;
        memcpy(&f, flat + i, 8);
        if (f == 0x0101010101010101ull)
            blit_span(R, fx0 + i, 8, ppx, ppy, out + i);
        else
            easu_core(R, fx0 + i, 8, ppx, ppy, out + i);
    }
    if (i < n)
        easu_core(R, fx0 + i, n - i, ppx, ppy, out + i);
}

// One 2x luma row `oy` into out (ow floats). `ph` is scratch, 2 * w floats.
static void scale_row(const VdUpscaler *u, int mode, int oy, float *restrict out,
                      float *restrict ph)
{
    const int w = u->w, s = u->stride;
    const int odd = oy & 1;
    const int fy = odd ? (oy - 1) / 2 : oy / 2 - 1;
    Rows R;
    for (int k = 0; k < 4; ++k)
        R.l[k] = u->l + (size_t)clampi(fy - 1 + k, 0, u->h - 1) * s;
    for (int k = 0; k < 2; ++k)
    {
        const size_t o = (size_t)clampi(fy + k, 0, u->h - 1) * s;
        R.dx[k] = u->dx + o;
        R.dy[k] = u->dy + o;
        R.len[k] = u->len + o;
    }
    R.flat = u->flat + (size_t)(clampi(fy, -1, u->h - 1) + 1) * s;
    // Phase 0.75 -> even columns (fx -1..w-2), phase 0.25 -> odd (fx 0..w-1).
    float *ev = ph, *od = ph + w;
    if (mode == VD_UPSCALE_FSR)
    {
        if (odd)
        {
            easu_span(&R, -1, w, 0.75f, 0.25f, ev);
            easu_span(&R, 0, w, 0.25f, 0.25f, od);
        }
        else
        {
            easu_span(&R, -1, w, 0.75f, 0.75f, ev);
            easu_span(&R, 0, w, 0.25f, 0.75f, od);
        }
    }
    else
    {
        if (odd)
        {
            blit_span(&R, -1, w, 0.75f, 0.25f, ev);
            blit_span(&R, 0, w, 0.25f, 0.25f, od);
        }
        else
        {
            blit_span(&R, -1, w, 0.75f, 0.75f, ev);
            blit_span(&R, 0, w, 0.25f, 0.75f, od);
        }
    }
    for (int i = 0; i < w; ++i)
    {
        out[2 * i] = ev[i];
        out[2 * i + 1] = od[i];
    }
}

// ---- sharpen (RCAS on luma, full sharpness) + colour -----------------------------

#define RCAS_LIMIT (0.25f - (1.0f / 16.0f))

// FsrRcasF on one channel: the cross b (up), d (left), e, f (right), h (down).
// `con` is the FsrRcasCon strength; `denoise` is FSR_RCAS_DENOISE (as FSR 2
// builds it), which backs the sharpening off where the centre stands out
// from its ring like noise instead of an edge.
static inline __attribute__((always_inline)) float rcas(float b, float d, float e, float f,
                                                        float h, float con, const int denoise)
{
    const float mn4 = minf(minf(b, d), minf(f, h));
    const float mx4 = maxf(maxf(b, d), maxf(f, h));
    const float hitMin = minf(mn4, e) * (1.0f / maxf(4.0f * mx4, 1e-6f));
    const float hitMax = (1.0f - maxf(mx4, e)) * (1.0f / minf(4.0f * mn4 - 4.0f, -1e-6f));
    const float lobe = maxf(-hitMin, hitMax);
    float wl = maxf(-RCAS_LIMIT, minf(lobe, 0.0f)) * con;
    if (denoise)
    {
        float nz = 0.25f * (b + d + f + h) - e;
        const float range = maxf(mx4, e) - minf(mn4, e);
        nz = clampf(absf(nz) * (1.0f / maxf(range, 1e-6f)), 0.0f, 1.0f);
        wl *= -0.5f * nz + 1.0f;
    }
    return (wl * (b + d + f + h) + e) * (1.0f / (4.0f * wl + 1.0f));
}

static inline __attribute__((always_inline)) void
sharpen_row_k(int ow, const float *restrict up, const float *restrict mid,
              const float *restrict down, float *restrict out, float con, const int denoise)
{
    out[0] = rcas(up[0], mid[0], mid[0], mid[1], down[0], con, denoise);
    for (int x = 1; x < ow - 1; ++x)
        out[x] = rcas(up[x], mid[x - 1], mid[x], mid[x + 1], down[x], con, denoise);
    out[ow - 1] = rcas(up[ow - 1], mid[ow - 2], mid[ow - 1], mid[ow - 1], down[ow - 1], con,
                       denoise);
}

static void sharpen_row(const VdUpscaler *u, const float *restrict up, const float *restrict mid,
                        const float *restrict down, float *restrict out)
{
    if (u->rcas_denoise)
        sharpen_row_k(u->ow, up, mid, down, out, u->rcas_con, 1);
    else
        sharpen_row_k(u->ow, up, mid, down, out, u->rcas_con, 0);
}

// Chroma terms for 2x row `sy`, bilinear from the half-resolution chroma
// plane (4x in each direction), into cr/cg/cb (ow floats each). `tmp` holds
// 3 * (cw + 2) floats.
static void chroma_row(const VdUpscaler *u, int sy, float *restrict cr, float *restrict cg,
                       float *restrict cb, float *restrict tmp)
{
    const float v = ((float)sy + 0.5f) * 0.25f - 0.5f;
    const float fv = floorf(v);
    const float t = v - fv;
    const int c0 = clampi((int)fv, 0, u->ch - 1), c1 = clampi((int)fv + 1, 0, u->ch - 1);
    const int cs = u->cstride, cw = u->cw;
    const float *planes[3] = {u->cr, u->cg, u->cb};
    float *outs[3] = {cr, cg, cb};
    for (int c = 0; c < 3; ++c)
    {
        const float *a = planes[c] + (size_t)c0 * cs - 1, *b = planes[c] + (size_t)c1 * cs - 1;
        float *row = tmp + (size_t)c * cs; // row[1 + k] = chroma column k, padded
        for (int k = 0; k < cw + 2; ++k)
            row[k] = a[k] + (b[k] - a[k]) * t;
        const float *m = row + 1;
        float *o = outs[c];
        for (int k = 0; k < cw; ++k)
        {
            const float l = m[k - 1], c0v = m[k], r = m[k + 1];
            o[4 * k + 0] = l + (c0v - l) * 0.625f;
            o[4 * k + 1] = l + (c0v - l) * 0.875f;
            o[4 * k + 2] = c0v + (r - c0v) * 0.125f;
            o[4 * k + 3] = c0v + (r - c0v) * 0.375f;
        }
    }
}

static inline uint32_t to8(float v)
{
    return (uint32_t)(int)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static void pack_row(int ow, const float *restrict l, const float *restrict cr,
                     const float *restrict cg, const float *restrict cb, uint32_t *restrict out)
{
    for (int x = 0; x < ow; ++x)
        out[x] = 0xFF000000u | (to8(l[x] + cr[x]) << 16) | (to8(l[x] + cg[x]) << 8) |
                 to8(l[x] + cb[x]);
}

int vd_upscale_render(VdUpscaler *u, int mode, uint32_t *dst, int pitch, int row_offset,
                      int dst_h, int begin, int end)
{
    begin = clampi(begin, 0, dst_h);
    end = clampi(end, 0, dst_h);
    if (begin >= end)
        return 0;
    const int ow = u->ow, oh = u->oh;
    // Ring of three 2x luma rows, the sharpened row, three chroma rows, scratch.
    const size_t n = (size_t)ow * 7 + (size_t)2 * u->w + (size_t)3 * u->cstride;
    float *mem = (float *)malloc(n * sizeof(float));
    if (!mem)
        return -1;
    float *slot[3] = {mem, mem + ow, mem + 2 * (size_t)ow};
    float *sharp = mem + 3 * (size_t)ow;
    float *cr = mem + 4 * (size_t)ow, *cg = mem + 5 * (size_t)ow, *cb = mem + 6 * (size_t)ow;
    float *ph = mem + 7 * (size_t)ow;
    float *ctmp = ph + 2 * (size_t)u->w;
    int ring_row[3] = {-1, -1, -1}; // which 2x row each slot holds
    for (int y = begin; y < end; ++y)
    {
        const int sy = y + row_offset; // 2x row
        if (sy < 0 || sy >= oh)
            continue;
        const int need[3] = {sy > 0 ? sy - 1 : 0, sy, sy < oh - 1 ? sy + 1 : oh - 1};
        int at[3];
        for (int i = 0; i < 3; ++i)
        {
            at[i] = -1;
            for (int k = 0; k < 3; ++k)
                if (ring_row[k] == need[i])
                    at[i] = k;
            if (at[i] >= 0)
                continue;
            // Reuse a slot none of the three rows needs.
            int k = 0;
            while (k < 3 && (ring_row[k] == need[0] || ring_row[k] == need[1] ||
                             ring_row[k] == need[2]))
                ++k;
            scale_row(u, mode, need[i], slot[k], ph);
            ring_row[k] = need[i];
            at[i] = k;
        }
        const float *luma = slot[at[1]];
        if (u->rcas_con > 0.0f)
        {
            sharpen_row(u, slot[at[0]], slot[at[1]], slot[at[2]], sharp);
            luma = sharp;
        }
        chroma_row(u, sy, cr, cg, cb, ctmp);
        pack_row(ow, luma, cr, cg, cb, dst + (size_t)y * pitch);
    }
    free(mem);
    return 0;
}

int vd_upscale_frame(VdUpscaler *u, const uint8_t *nv12, int mode, uint32_t *dst, int pitch,
                     int row_offset, int dst_h)
{
    vd_upscale_convert(u, nv12, 0, u->h);
    if (mode == VD_UPSCALE_FSR)
        vd_upscale_analyze(u, 0, u->h);
    return vd_upscale_render(u, mode, dst, pitch, row_offset, dst_h, 0, dst_h);
}
