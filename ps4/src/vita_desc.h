// VITA4 - PS Vita USB descriptor parsing from a raw configuration descriptor.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same classification as the PS5 payload's core/src/usb_vita.c, but working on
// a raw config-descriptor buffer (the PS4 reads it through sceUsbd instead of
// FreeBSD ugen ioctls). Pure C, host-tested.
#pragma once
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

#include "usb_vita.h" // VdUsbVitaDevice (core/include)

#define VD_VITA_VID 0x054Cu
#define VD_VITA_PID 0x1338u

// Parse a full configuration descriptor (wTotalLength bytes). Fills `dev`
// (vid/pid/path are left for the caller). Returns 0 when the descriptor
// exposes a UVC VideoStreaming interface with a bulk IN endpoint.
int vd_parse_vita_config(const uint8_t *data, unsigned total, VdUsbVitaDevice *dev);

// Find the frame descriptor whose NV12 size equals `payload_bytes`.
// Prefers the frame index `preferred` when several match. Returns 1 and the
// size on a match, 0 otherwise.
int vd_match_nv12_frame(const VdUsbVitaDevice *dev, uint32_t payload_bytes,
                        uint8_t preferred, uint16_t *w, uint16_t *h);

// Largest NV12 frame (bytes) any parsed frame descriptor advertises; 0 if none.
uint32_t vd_largest_nv12_frame(const VdUsbVitaDevice *dev);

// First NV12 format index and its frame descriptor for the Vita's native
// 960x544 mode (or the first frame if absent). Returns 0 on success.
int vd_pick_default_frame(const VdUsbVitaDevice *dev, uint8_t *format_index,
                          const VdUvcFrame **frame);
// Same, for a wanted size (1.30: 480x272 "Fast" picture); falls back to the
// default pick when the Vita doesn't offer it.
int vd_pick_frame(const VdUsbVitaDevice *dev, int want_w, int want_h, uint8_t *format_index,
                  const VdUvcFrame **frame);

#ifdef __cplusplus
}
#endif
