// VITA4 - PS Vita USB descriptor parsing. See vita_desc.h.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interface walk mirrors core/src/usb_vita.c (vd_usb_inspect_vita) so the
// PS4 and PS5 builds classify the Vita identically.
#include "vita_desc.h"

#include <string.h>

#include "uvc_protocol.h"

static int is_video_streaming_iface(uint8_t c, uint8_t s)
{
    return c == USB_CLASS_VIDEO && s == UVC_SC_VIDEOSTREAMING;
}
static int is_video_control_iface(uint8_t c, uint8_t s)
{
    return c == USB_CLASS_VIDEO && s == UVC_SC_VIDEOCONTROL;
}
static int is_audio_iface(uint8_t c, uint8_t s)
{
    return c == USB_CLASS_AUDIO && (s == 0x01 || s == 0x02);
}
static int is_pad_iface(uint8_t c, uint8_t s, uint8_t p)
{
    return c == 0xFF && s == 0x50 && p == 0x01;
}

static void collect_endpoints(const uint8_t *iface, unsigned iface_len, uint8_t *in_eps,
                              unsigned *in_count, uint8_t *out_eps, unsigned *out_count)
{
    unsigned pos = iface[0];
    while (pos + 2 <= iface_len)
    {
        const uint8_t *d = iface + pos;
        if (d[0] < 2 || pos + d[0] > iface_len)
            break;
        if (d[1] == USB_DT_INTERFACE)
            break;
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7)
        {
            uint8_t addr = d[2];
            if (addr & 0x80)
            {
                if (*in_count < VD_USB_MAX_ENDPOINTS)
                    in_eps[(*in_count)++] = addr;
            }
            else if (*out_count < VD_USB_MAX_ENDPOINTS)
            {
                out_eps[(*out_count)++] = addr;
            }
        }
        pos += d[0];
    }
}

static int find_isoc_in_endpoint(const uint8_t *iface, unsigned iface_len, uint8_t *addr_out,
                                 uint16_t *maxpkt_out)
{
    unsigned pos = iface[0];
    while (pos + 2 <= iface_len)
    {
        const uint8_t *d = iface + pos;
        if (d[0] < 2 || pos + d[0] > iface_len)
            break;
        if (d[1] == USB_DT_INTERFACE)
            break;
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7)
        {
            if ((d[2] & 0x80) && (d[3] & 0x03) == 0x01)
            {
                *addr_out = d[2];
                *maxpkt_out = (uint16_t)(d[4] | ((uint16_t)d[5] << 8));
                return 1;
            }
        }
        pos += d[0];
    }
    return 0;
}

static void parse_vs(const uint8_t *config, unsigned config_len, unsigned iface_offset,
                     unsigned iface_len, uint8_t iface_number, VdUsbVitaDevice *dev)
{
    uint8_t blob[4096];
    unsigned blob_len = 0;
    const unsigned extent_end = iface_offset + iface_len;
    unsigned pos = iface_offset + config[iface_offset];
    while (pos + 3 <= extent_end && pos + 3 <= config_len)
    {
        const uint8_t *d = config + pos;
        if (d[0] < 2 || pos + d[0] > config_len)
            break;
        if (d[1] == USB_DT_INTERFACE || d[1] == USB_DT_IAD)
            break;
        if (d[1] == USB_DT_CS_INTERFACE)
        {
            if (blob_len + d[0] > sizeof(blob))
                break;
            memcpy(blob + blob_len, d, d[0]);
            blob_len += d[0];
        }
        pos += d[0];
    }
    if (blob_len == 0)
        return;
    VdUvcStreaming streaming;
    if (vd_uvc_parse_streaming(blob, blob_len, iface_number, &streaming) == 0 &&
        dev->uvc.interface_number == 0xFF)
    {
        dev->uvc = streaming;
        dev->has_video = 1;
    }
}

int vd_parse_vita_config(const uint8_t *data, unsigned total, VdUsbVitaDevice *dev)
{
    if (!data || !dev || total < 9)
        return -1;
    // Keep caller-provided identity fields.
    char path[sizeof(dev->path)];
    uint16_t vid = dev->vid, pid = dev->pid;
    uint8_t cls = dev->device_class;
    memcpy(path, dev->path, sizeof(path));
    memset(dev, 0, sizeof(*dev));
    memcpy(dev->path, path, sizeof(path));
    dev->vid = vid;
    dev->pid = pid;
    dev->device_class = cls;
    dev->uvc.interface_number = 0xFF;
    dev->uac.interface_number = 0xFF;

    unsigned pos = 0;
    while (pos + 2 <= total)
    {
        const uint8_t *d = data + pos;
        if (d[0] < 2 || pos + d[0] > total)
            break;
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9)
        {
            const uint8_t iface_number = d[2];
            const uint8_t alt = d[3];
            const uint8_t class_code = d[5];
            const uint8_t subclass = d[6];
            const uint8_t protocol = d[7];

            unsigned extent = pos;
            while (extent + 2 <= total)
            {
                const uint8_t *e = data + extent;
                if (e[0] < 2 || extent + e[0] > total)
                    break;
                if (extent != pos && (e[1] == USB_DT_INTERFACE || e[1] == USB_DT_IAD))
                    break;
                extent += e[0];
            }
            const unsigned iface_len = extent - pos;

            if (alt == 0 && is_video_streaming_iface(class_code, subclass))
            {
                collect_endpoints(data + pos, iface_len, dev->video_in_eps, &dev->video_in_count,
                                  dev->video_out_eps, &dev->video_out_count);
                parse_vs(data, total, pos, iface_len, iface_number, dev);
                if (dev->uvc.interface_number == 0xFF)
                    dev->uvc.interface_number = iface_number;
            }
            else if (alt == 0 && is_video_control_iface(class_code, subclass))
            {
                dev->video_control_interface = iface_number;
                dev->has_video_control = 1;
            }
            else if (is_audio_iface(class_code, subclass))
            {
                if (alt == 0)
                {
                    collect_endpoints(data + pos, iface_len, dev->audio_in_eps,
                                      &dev->audio_in_count, dev->audio_out_eps,
                                      &dev->audio_out_count);
                    dev->uac.interface_number = iface_number;
                    dev->has_audio = 1;
                }
                if (subclass == 0x02)
                    dev->audio_stream_interface = iface_number;
                uint8_t ep = 0;
                uint16_t maxpkt = 0;
                if (find_isoc_in_endpoint(data + pos, iface_len, &ep, &maxpkt))
                {
                    dev->audio_isoc_ep = ep;
                    dev->audio_isoc_maxpkt = maxpkt;
                    dev->audio_isoc_alt = alt;
                    dev->audio_stream_interface = iface_number;
                }
            }
            else if (alt == 0 && is_pad_iface(class_code, subclass, protocol))
            {
                uint8_t in_eps[VD_USB_MAX_ENDPOINTS], out_eps[VD_USB_MAX_ENDPOINTS];
                unsigned in_n = 0, out_n = 0;
                collect_endpoints(data + pos, iface_len, in_eps, &in_n, out_eps, &out_n);
                if (out_n > 0)
                {
                    dev->pad_interface = iface_number;
                    dev->pad_out_ep = out_eps[0];
                    dev->has_pad = 1;
                }
            }
            pos = extent;
            continue;
        }
        pos += d[0];
    }

    if (dev->has_video && dev->video_in_count > 0 && dev->uvc.interface_number != 0xFF)
    {
        dev->is_vita = 1;
        return 0;
    }
    return -1;
}

int vd_match_nv12_frame(const VdUsbVitaDevice *dev, uint32_t payload_bytes, uint8_t preferred,
                        uint16_t *w, uint16_t *h)
{
    int found = 0;
    for (unsigned fi = 0; fi < dev->uvc.format_count; ++fi)
    {
        const VdUvcFormat *f = &dev->uvc.formats[fi];
        if (!f->is_nv12)
            continue;
        for (unsigned i = 0; i < f->frame_count; ++i)
        {
            const VdUvcFrame *fr = &f->frames[i];
            if (VD_NV12_FRAME_SIZE(fr->width, fr->height) != payload_bytes)
                continue;
            if (!found || fr->frame_index == preferred)
            {
                *w = fr->width;
                *h = fr->height;
                found = 1;
            }
        }
    }
    if (!found && payload_bytes == VD_NV12_FRAME_SIZE(960, 544))
    {
        // Descriptors unavailable: the Vita's native mode is the safe default.
        *w = 960;
        *h = 544;
        found = 1;
    }
    return found;
}

uint32_t vd_largest_nv12_frame(const VdUsbVitaDevice *dev)
{
    uint32_t best = 0;
    for (unsigned fi = 0; fi < dev->uvc.format_count; ++fi)
    {
        const VdUvcFormat *f = &dev->uvc.formats[fi];
        if (!f->is_nv12)
            continue;
        for (unsigned i = 0; i < f->frame_count; ++i)
        {
            uint32_t sz = VD_NV12_FRAME_SIZE(f->frames[i].width, f->frames[i].height);
            if (sz > best)
                best = sz;
        }
    }
    return best;
}

int vd_pick_frame(const VdUsbVitaDevice *dev, int want_w, int want_h, uint8_t *format_index,
                  const VdUvcFrame **frame)
{
    for (unsigned fi = 0; fi < dev->uvc.format_count; ++fi)
    {
        const VdUvcFormat *f = &dev->uvc.formats[fi];
        if (!f->is_nv12)
            continue;
        for (unsigned i = 0; i < f->frame_count; ++i)
        {
            if (f->frames[i].width == want_w && f->frames[i].height == want_h)
            {
                *format_index = f->format_index;
                *frame = &f->frames[i];
                return 0;
            }
        }
    }
    return vd_pick_default_frame(dev, format_index, frame);
}

int vd_pick_default_frame(const VdUsbVitaDevice *dev, uint8_t *format_index,
                          const VdUvcFrame **frame)
{
    const VdUvcFormat *first = NULL;
    for (unsigned fi = 0; fi < dev->uvc.format_count; ++fi)
    {
        const VdUvcFormat *f = &dev->uvc.formats[fi];
        if (!f->is_nv12 || f->frame_count == 0)
            continue;
        if (!first)
            first = f;
        for (unsigned i = 0; i < f->frame_count; ++i)
        {
            if (f->frames[i].width == 960 && f->frames[i].height == 544)
            {
                *format_index = f->format_index;
                *frame = &f->frames[i];
                return 0;
            }
        }
    }
    if (!first)
        return -1;
    *format_index = first->format_index;
    *frame = &first->frames[0];
    return 0;
}
