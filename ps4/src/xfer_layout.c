// VITA4 - libusb_transfer layout detection. See xfer_layout.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xfer_layout.h"

#include <string.h>

static int32_t rd_i32(const void *base, size_t off)
{
    int32_t v;
    memcpy(&v, (const uint8_t *)base + off, sizeof(v));
    return v;
}
static uint32_t rd_u32(const void *base, size_t off)
{
    uint32_t v;
    memcpy(&v, (const uint8_t *)base + off, sizeof(v));
    return v;
}
static void *rd_ptr(const void *base, size_t off)
{
    void *v;
    memcpy(&v, (const uint8_t *)base + off, sizeof(v));
    return v;
}
static void wr_u32(void *base, size_t off, uint32_t v)
{
    memcpy((uint8_t *)base + off, &v, sizeof(v));
}
static void wr_i32(void *base, size_t off, int32_t v)
{
    memcpy((uint8_t *)base + off, &v, sizeof(v));
}
static void wr_ptr(void *base, size_t off, const void *v)
{
    memcpy((uint8_t *)base + off, &v, sizeof(v));
}

static void set_upstream(VdXferLayout *L)
{
    L->kind = VD_XFER_LAYOUT_UPSTREAM;
    L->off_handle = 0;
    L->off_flags = 8;
    L->off_endpoint = 9;
    L->endpoint_bytes = 1;
    L->off_type = 10;
    L->off_timeout = 12;
    L->off_status = 16;
    L->off_length = 20;
    L->off_actual = 24;
    L->off_callback = 32;
    L->off_user = 40;
    L->off_buffer = 48;
    L->off_num_iso = 56;
    L->off_iso = 60;
}

static void set_freebsd(VdXferLayout *L)
{
    L->kind = VD_XFER_LAYOUT_FREEBSD;
    L->off_handle = 0;
    L->off_flags = 8;
    L->off_endpoint = 12;
    L->endpoint_bytes = 4;
    L->off_type = 16;
    L->off_timeout = 20;
    L->off_status = 24;
    L->off_length = 28;
    L->off_actual = 32;
    L->off_callback = 40;
    L->off_user = 48;
    L->off_buffer = 56;
    L->off_num_iso = 64;
    L->off_iso = 68;
}

int vd_xfer_layout_detect(const void *t, int num_iso, VdXferLayout *out)
{
    memset(out, 0, sizeof(*out));
    if (!t || num_iso <= 0)
        return VD_XFER_LAYOUT_NONE;
    // Upstream: num_iso at 56, and the buffer pointer before it is unset.
    const int up = rd_i32(t, 56) == num_iso && rd_ptr(t, 48) == NULL;
    // FreeBSD: num_iso at 64, buffer pointer at 56 unset.
    const int bsd = rd_i32(t, 64) == num_iso && rd_ptr(t, 56) == NULL;
    if (up && !bsd)
        set_upstream(out);
    else if (bsd && !up)
        set_freebsd(out);
    return out->kind;
}

void vd_xfer_fill_iso(const VdXferLayout *L, void *t, void *dev_handle, uint8_t endpoint,
                      uint8_t *buffer, int num_iso, unsigned packet_len, void *callback,
                      void *user_data, unsigned timeout_ms)
{
    wr_ptr(t, L->off_handle, dev_handle);
    ((uint8_t *)t)[L->off_flags] = 0;
    if (L->endpoint_bytes == 1)
        ((uint8_t *)t)[L->off_endpoint] = endpoint;
    else
        wr_u32(t, L->off_endpoint, endpoint);
    ((uint8_t *)t)[L->off_type] = VD_XFER_TYPE_ISOCHRONOUS;
    wr_u32(t, L->off_timeout, timeout_ms);
    wr_i32(t, L->off_status, 0);
    wr_i32(t, L->off_length, (int32_t)(packet_len * (unsigned)num_iso));
    wr_i32(t, L->off_actual, 0);
    wr_ptr(t, L->off_callback, callback);
    wr_ptr(t, L->off_user, user_data);
    wr_ptr(t, L->off_buffer, buffer);
    wr_i32(t, L->off_num_iso, num_iso);
    for (int i = 0; i < num_iso; ++i)
    {
        const size_t d = L->off_iso + (size_t)i * VD_ISO_DESC_BYTES;
        wr_u32(t, d + 0, packet_len);
        wr_u32(t, d + 4, 0);
        wr_i32(t, d + 8, 0);
    }
}

int vd_xfer_status(const VdXferLayout *L, const void *t)
{
    return rd_i32(t, L->off_status);
}
void *vd_xfer_user_data(const VdXferLayout *L, const void *t)
{
    return rd_ptr(t, L->off_user);
}
uint8_t *vd_xfer_buffer(const VdXferLayout *L, const void *t)
{
    return (uint8_t *)rd_ptr(t, L->off_buffer);
}
int vd_xfer_num_iso(const VdXferLayout *L, const void *t)
{
    return rd_i32(t, L->off_num_iso);
}
unsigned vd_xfer_iso_length(const VdXferLayout *L, const void *t, int i)
{
    return rd_u32(t, L->off_iso + (size_t)i * VD_ISO_DESC_BYTES + 0);
}
unsigned vd_xfer_iso_actual(const VdXferLayout *L, const void *t, int i)
{
    return rd_u32(t, L->off_iso + (size_t)i * VD_ISO_DESC_BYTES + 4);
}
int vd_xfer_iso_status(const VdXferLayout *L, const void *t, int i)
{
    return rd_i32(t, L->off_iso + (size_t)i * VD_ISO_DESC_BYTES + 8);
}
