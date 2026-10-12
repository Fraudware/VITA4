// VITA4 - runtime detection of the libusb_transfer layout used by sceUsbd.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS4's libSceUsbd is a libusb-1.0 implementation, but no public header
// pins down its `struct libusb_transfer`. Two layouts exist in the wild:
//
//   upstream libusb       FreeBSD libusb (the PS4 OS is FreeBSD-based)
//   0  dev_handle *       0  dev_handle *
//   8  u8 flags           8  u8 flags
//   9  u8 endpoint        12 u32 endpoint
//   10 u8 type            16 u8 type
//   12 u32 timeout        20 u32 timeout
//   16 status             24 status
//   20 length             28 length
//   24 actual_length      32 actual_length
//   32 callback           40 callback
//   40 user_data          48 user_data
//   48 buffer             56 buffer
//   56 num_iso_packets    64 num_iso_packets
//   60 iso_packet_desc[]  68 iso_packet_desc[]   ({length, actual_length, status})
//
// sceUsbdAllocTransfer(n) stores n in num_iso_packets, so the layout is
// identified by where that value lands. Audio (isochronous) transfers are
// then filled and read through these offsets; if neither layout matches,
// audio is switched off instead of guessing. Pure C, host-tested.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    VD_XFER_LAYOUT_NONE = 0,
    VD_XFER_LAYOUT_UPSTREAM = 1,
    VD_XFER_LAYOUT_FREEBSD = 2,
};

enum
{
    VD_XFER_TYPE_ISOCHRONOUS = 1,
};

enum
{
    VD_XFER_COMPLETED = 0,
    VD_XFER_ERROR = 1,
    VD_XFER_TIMED_OUT = 2,
    VD_XFER_CANCELLED = 3,
    VD_XFER_STALL = 4,
    VD_XFER_NO_DEVICE = 5,
    VD_XFER_OVERFLOW = 6,
};

typedef struct
{
    int kind;
    size_t off_handle, off_flags, off_endpoint, endpoint_bytes, off_type, off_timeout;
    size_t off_status, off_length, off_actual, off_callback, off_user, off_buffer;
    size_t off_num_iso, off_iso;
} VdXferLayout;

#define VD_ISO_DESC_BYTES 12u

// Inspect a freshly allocated transfer created with `num_iso` packets
// (num_iso must be non-zero). Returns the detected kind (NONE on failure).
int vd_xfer_layout_detect(const void *transfer, int num_iso, VdXferLayout *out);

// Fill an isochronous IN transfer: every packet gets `packet_len` bytes.
void vd_xfer_fill_iso(const VdXferLayout *L, void *t, void *dev_handle, uint8_t endpoint,
                      uint8_t *buffer, int num_iso, unsigned packet_len, void *callback,
                      void *user_data, unsigned timeout_ms);

int vd_xfer_status(const VdXferLayout *L, const void *t);
void *vd_xfer_user_data(const VdXferLayout *L, const void *t);
uint8_t *vd_xfer_buffer(const VdXferLayout *L, const void *t);
int vd_xfer_num_iso(const VdXferLayout *L, const void *t);
unsigned vd_xfer_iso_length(const VdXferLayout *L, const void *t, int i);
unsigned vd_xfer_iso_actual(const VdXferLayout *L, const void *t, int i);
int vd_xfer_iso_status(const VdXferLayout *L, const void *t, int i);

#ifdef __cplusplus
}
#endif
