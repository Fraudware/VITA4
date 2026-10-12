/* SPDX-License-Identifier: GPL-3.0-or-later
 * VITA5 — shared-memory contract between the kernel USB payload and the
 * native display app. One physical page region, mapped by both sides.
 *
 * Layout (single mmap, fixed offsets):
 *   VdSharedHeader   at offset 0
 *   video ring       at VD_SHM_VIDEO_OFFSET
 *   audio ring       at VD_SHM_AUDIO_OFFSET
 *   pad ring         at VD_SHM_PAD_OFFSET
 *
 * Synchronization: each ring is a single-producer/single-consumer slot ring
 * with a release/acquire sequence counter per slot (no locks, no syscalls).
 */
#pragma once
#include <stdint.h>

#define VD_SHM_MAGIC             0x56444F4Bu /* 'VDOK' */
#define VD_SHM_VERSION           2u

/* Ring sizing: video double-buffer (2 slots of max 720p NV12), audio PCM
 * ring, pad ring. All offsets are byte offsets from the region base. */
#define VD_SHM_HEADER_SIZE       4096u

#define VD_VIDEO_MAX_FRAME       (1280u * 720u * 3u / 2u) /* 1,382,400 */
#define VD_VIDEO_SLOTS           2u
#define VD_SHM_VIDEO_OFFSET      VD_SHM_HEADER_SIZE
#define VD_SHM_VIDEO_SLOT_SIZE   VD_VIDEO_MAX_FRAME
#define VD_SHM_VIDEO_BYTES       (VD_VIDEO_SLOTS * VD_SHM_VIDEO_SLOT_SIZE)

#define VD_AUDIO_CHUNK_BYTES     4096u /* 256 stereo frames @48k 16-bit */
#define VD_AUDIO_SLOTS           16u
#define VD_SHM_AUDIO_OFFSET      (VD_SHM_VIDEO_OFFSET + VD_SHM_VIDEO_BYTES)
#define VD_SHM_AUDIO_BYTES       (VD_AUDIO_SLOTS * VD_AUDIO_CHUNK_BYTES)

#define VD_PAD_REPORT_BYTES      64u
#define VD_PAD_SLOTS             8u
/* Touch state (filled by app, read by kernel -> forwarded to the Vita).
 * Touch records REPLACE state (docs/VITA_INPUT_API.md section 10), so no
 * slot ring is needed: one latest-wins 16-byte wire record plus a sequence
 * the publisher bumps AFTER filling the record. (Same value as
 * pad_passthrough.h's VD_TOUCH_WIRE_BYTES.) */
#ifndef VD_TOUCH_WIRE_BYTES
#define VD_TOUCH_WIRE_BYTES      16u
#endif

#define VD_SHM_PAD_OFFSET        (VD_SHM_AUDIO_OFFSET + VD_SHM_AUDIO_BYTES)
#define VD_SHM_PAD_BYTES         (VD_PAD_SLOTS * VD_PAD_REPORT_BYTES)

#define VD_SHM_TOTAL_BYTES       (VD_SHM_PAD_OFFSET + VD_SHM_PAD_BYTES)

/* Per-slot publish state. Producer bumps `seq` to an ODD value before filling,
 * writes the payload, then bumps to the NEXT EVEN value as the release. A
 * consumer reads `seq`, copies the slot, and re-reads `seq` to confirm it did
 * not change mid-copy (a change means a torn frame -> drop and retry). */
typedef struct {
    volatile uint32_t seq;
    uint32_t _pad;
} VdSlotState;

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* Video state (filled by kernel, read by app) */
    uint32_t video_width;
    uint32_t video_height;
    uint32_t video_pixel_format; /* VD_PIX_* */
    uint32_t video_slot_seq[VD_VIDEO_SLOTS]; /* even = ready */
    volatile uint32_t video_producer;        /* next slot to write */

    /* Audio state (filled by kernel, read by app) */
    uint32_t audio_sample_rate;  /* 48000 */
    uint32_t audio_channels;     /* 2 */
    uint32_t audio_bits;         /* 16 */
    uint32_t audio_slot_seq[VD_AUDIO_SLOTS];
    volatile uint32_t audio_producer;

    /* Pad state (filled by app, read by kernel -> forwarded to Vita) */
    uint32_t pad_slot_seq[VD_PAD_SLOTS];
    volatile uint32_t pad_producer;

    /* Touch state (filled by app, read by kernel -> forwarded to Vita).
     * Latest-wins: the app fills touch_wire (vd_touch_serialize output),
     * then bumps touch_seq to an even value; the kernel forwards any
     * record whose seq changed since it last looked. */
    volatile uint32_t touch_seq;
    volatile uint32_t touch_reports;
    uint8_t touch_wire[VD_TOUCH_WIRE_BYTES];

    /* Control/health */
    volatile uint32_t vita_detected;
    volatile uint32_t stream_active;
    volatile uint32_t frames_captured;
    volatile uint32_t frames_dropped;
    volatile uint32_t audio_chunks;
    volatile uint32_t pad_reports;
    volatile uint32_t last_error; /* VD_ERR_* */
    uint32_t _reserved[18];
} VdSharedHeader;

/* Pixel formats */
#define VD_PIX_NONE   0u
#define VD_PIX_NV12   1u

/* Error codes */
#define VD_ERR_NONE               0u
#define VD_ERR_NO_VITA            1u
#define VD_ERR_PROBE_FAILED       2u
#define VD_ERR_COMMIT_FAILED      3u
#define VD_ERR_BULK_READ_FAILED   4u
#define VD_ERR_BAD_FRAME_HEADER   5u
#define VD_ERR_VITA_DISCONNECTED  6u

/* Offsets for the app's mmap */
#define VD_SHM_VIDEO_PTR(base, slot) \
    ((uint8_t *)(base) + VD_SHM_VIDEO_OFFSET + (slot) * VD_SHM_VIDEO_SLOT_SIZE)
#define VD_SHM_AUDIO_PTR(base, slot) \
    ((uint8_t *)(base) + VD_SHM_AUDIO_OFFSET + (slot) * VD_AUDIO_CHUNK_BYTES)
#define VD_SHM_PAD_PTR(base, slot) \
    ((uint8_t *)(base) + VD_SHM_PAD_OFFSET + (slot) * VD_PAD_REPORT_BYTES)
