// VITA4 - CPU port of VITA5's video upscale chain (Sharp / FSR).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// VITA5 (app/src/vd/upscaler.cpp + upscale_shaders.hpp) runs these passes as
// GL shaders. PS4 homebrew can only load precompiled shader binaries, so
// VITA4 runs the same math on the CPU, split into row bands across threads:
//
//   convert  NV12 -> RGB (BT.601 limited, VITA5 kYuv) + luma
//   analyze  per-texel EASU direction/length terms (FSR only)
//   scale    2x: bilinear (Sharp, VITA5 kBlit) or AMD FSR 1.0 EASU (FSR)
//   sharpen  AMD FSR 1.0 RCAS at output size, full sharpness (VITA5 kRcas)
//
// The EASU/RCAS formulas follow VITA5's shader port of ffx_fsr1.h (AMD, MIT,
// see licenses/FSR1-MIT.txt). Two restructurings make it fast enough for a
// CPU without changing the result:
//  - EASU's per-quadrant direction/length terms (FsrEasuSetF) depend only on
//    the texel they are centred on, so they are computed once per source
//    texel and blended per output pixel (identical away from the outermost
//    ring of texels, where the shader's per-tap edge clamp differs slightly).
//  - At exactly 2x every output pixel sits at one of four sub-texel phases
//    (0.25 / 0.75 in x and y), so each phase is a straight loop with constant
//    offsets that the compiler vectorizes.
//
// The 2x image is 1920x1088 for the Vita's 960x544; render crops it to the
// screen. Pure C, host-tested.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum VdUpscaleMode
{
    VD_UPSCALE_OFF = 0,   // the plain path (nv12_scale.c)
    VD_UPSCALE_SHARP = 1, // bilinear 2x + RCAS
    VD_UPSCALE_FSR = 2,   // EASU 2x + RCAS
    VD_UPSCALE_SMOOTH = 3, // VITA4: byte-SIMD sharpen + bilinear 2x (smooth.h), full speed
    VD_UPSCALE_TV = 4,     // VITA4 test: the PS4's display hardware scales 960x544
    VD_UPSCALE_COUNT
};

// "Off" / "Smooth" / "TV scaler" / "Sharp" / "FSR" (Sharp/FSR: VITA5's labels)
const char *vd_upscale_label(int mode);
// The next mode in menu / L3 order: Off, Smooth, TV scaler, Sharp, FSR (the
// slow CPU ports last). dir = +1 / -1.
int vd_upscale_next(int mode, int dir);
// Sharp and FSR run in the background worker (upscale_worker.h).
int vd_upscale_uses_worker(int mode);

typedef struct VdUpscaler VdUpscaler;

// Working buffers for a source size (output is 2w x 2h). NULL on failure.
VdUpscaler *vd_upscaler_create(int src_w, int src_h);
void vd_upscaler_destroy(VdUpscaler *u);
int vd_upscaler_src_w(const VdUpscaler *u);

// RCAS sharpening after the scale, set between frames. `sharpness` is FSR 2's
// slider: 0 = no sharpening pass at all, (0, 1] -> FsrRcasCon with
// 2 - 2 * sharpness stops (1 = full, VITA5's setting; the default).
// `denoise` = FSR_RCAS_DENOISE, as FSR 2 enables it (default off).
void vd_upscaler_set_sharpness(VdUpscaler *u, float sharpness, int denoise);
int vd_upscaler_src_h(const VdUpscaler *u);

// The passes, in order. Each covers rows [begin, end) and may run on many
// threads at once; a pass must finish for all rows before the next starts.
//   convert, analyze: source rows (analyze is only needed for FSR)
//   render: rows of `dst`. Scales (bilinear or EASU) and sharpens (RCAS) in
//           one go, keeping the 2x rows in a small per-call ring so nothing
//           full-size round-trips through memory. Output row y comes from 2x
//           row y + row_offset (that is the crop: 1088 -> 1080 uses
//           row_offset 4, dst_h 1080), written as 0xFFRRGGBB.
void vd_upscale_convert(VdUpscaler *u, const uint8_t *nv12, int begin, int end);
void vd_upscale_analyze(VdUpscaler *u, int begin, int end);
// Returns 0, or -1 when its scratch rows can't be allocated (rows untouched).
int vd_upscale_render(VdUpscaler *u, int mode, uint32_t *dst, int pitch, int row_offset,
                      int dst_h, int begin, int end);

// All passes on the calling thread (tests, tools). Returns 0 / -1.
int vd_upscale_frame(VdUpscaler *u, const uint8_t *nv12, int mode, uint32_t *dst, int pitch,
                     int row_offset, int dst_h);

#ifdef __cplusplus
}
#endif
