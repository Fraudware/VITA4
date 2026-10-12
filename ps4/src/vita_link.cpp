// VITA4 - USB link to the PS Vita. See vita_link.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "vita_link.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"
extern "C" {
#include "uac_audio.h"
#include "usb_transfer.h"
#include "usb_vita.h"
#include "uvc_protocol.h"
#include "uvc_stream.h"
}

extern "C" {
// libusb_handle_events_timeout without the context argument.
int32_t sceUsbdHandleEventsTimeout(struct timeval *tv);
}

namespace vd4
{

namespace
{

constexpr uint32_t kCtrlTimeoutMs = 1000;
constexpr uint32_t kInputTimeoutMs = 100;
constexpr uint32_t kBulkTimeoutMs = 300;
constexpr uint32_t kChunk = 64 * 1024; // proven chunk size from the PS5 build
constexpr uint32_t kMaxPacket = 512;   // high-speed bulk

const char *err_name(int e)
{
    switch (e)
    {
        case VD_USB_ERR_IO:
            return "IO";
        case VD_USB_ERR_INVALID:
            return "INVALID";
        case VD_USB_ERR_ACCESS:
            return "ACCESS";
        case VD_USB_ERR_NO_DEVICE:
            return "NO_DEVICE";
        case VD_USB_ERR_NOT_FOUND:
            return "NOT_FOUND";
        case VD_USB_ERR_BUSY:
            return "BUSY";
        case VD_USB_ERR_TIMEOUT:
            return "TIMEOUT";
        case VD_USB_ERR_OVERFLOW:
            return "OVERFLOW";
        case VD_USB_ERR_PIPE:
            return "PIPE";
        case VD_USB_ERR_INTERRUPTED:
            return "INTERRUPTED";
        case VD_USB_ERR_NO_MEM:
            return "NO_MEM";
        case VD_USB_ERR_NOT_SUPPORTED:
            return "NOT_SUPPORTED";
        default:
            return e >= 0 ? "OK" : "?";
    }
}

} // namespace

// Raw /dev/ugen state: the PS5 build's core sessions, used as they are.
struct VitaLink::Ugen
{
    VdUsbVitaDevice dev{};
    VdUvcSession uvc{};
    bool uvc_open = false;
    int ctrl_fd = -1; // EP0 vendor requests (pad/touch), like the PS5 forwarder
    VdUacSession uac{};
    bool uac_open = false;
    std::thread uac_thread;
    std::atomic<bool> uac_stop{false};
};

int VitaLink::init()
{
    const int32_t rc = sceUsbdInit();
    log_line("sceUsbdInit -> 0x%08x", (unsigned)rc);
    return rc;
}

void VitaLink::shutdown()
{
    close();
    sceUsbdExit();
}

bool VitaLink::find_and_open(char *seen, int seen_cap)
{
    if (seen && seen_cap)
        seen[0] = 0;
    ++scans_;
    if (find_usbd(seen, seen_cap))
    {
        backend_ = Backend::usbd;
        log_line("connected through the PS4 USB library (sceUsbd)");
        return true;
    }
    if (ugen_allowed_ && find_ugen())
    {
        backend_ = Backend::ugen;
        log_line("connected through raw USB (%s, PS5 core code)", ugen_->dev.path);
        return true;
    }
    return false;
}

bool VitaLink::find_usbd(char *seen, int seen_cap)
{
    libusb_device **list = nullptr;
    const int32_t n = sceUsbdGetDeviceList(&list);
    char scan[512];
    int sp = snprintf(scan, sizeof(scan), "sceUsbd list -> %d (0x%08x):", n, (unsigned)n);
    libusb_device *match = nullptr;
    int used = 0;
    for (int32_t i = 0; list && i < n; ++i)
    {
        VdUsbDeviceDescriptor d;
        memset(&d, 0, sizeof(d));
        const int32_t drc = sceUsbdGetDeviceDescriptor(list[i], &d);
        if (sp < (int)sizeof(scan) - 32)
        {
            if (drc < 0)
                sp += snprintf(scan + sp, sizeof(scan) - (size_t)sp, " [desc err 0x%08x]",
                               (unsigned)drc);
            else
                sp += snprintf(scan + sp, sizeof(scan) - (size_t)sp, " %04x:%04x/%02x", d.idVendor,
                               d.idProduct, d.bDeviceClass);
        }
        if (drc < 0)
            continue;
        if (seen && used < seen_cap - 12)
            used += snprintf(seen + used, (size_t)(seen_cap - used), "%s%04x:%04x",
                             used ? " " : "", d.idVendor, d.idProduct);
        if (!match && d.idVendor == VD_VITA_VID && d.idProduct == VD_VITA_PID)
        {
            match = list[i];
            dev_.vid = d.idVendor;
            dev_.pid = d.idProduct;
            dev_.device_class = d.bDeviceClass;
        }
    }
    // Log the scan whenever it changes, and a heartbeat every ~30 scans.
    if (strcmp(scan, last_scan_) != 0 || scans_ % 30 == 1)
    {
        log_line("scan %u: %s", scans_, scan);
        snprintf(last_scan_, sizeof(last_scan_), "%s", scan);
    }

    libusb_device_handle *h = nullptr;
    int rc = 0;
    if (match)
    {
        rc = vd_usbd_err(sceUsbdOpen(match, &h));
    }
    else
    {
        // Not in the list: ask for it by ID anyway (some libusb ports only
        // list devices without a system driver).
        h = sceUsbdOpenDeviceWithVidPid(VD_VITA_VID, VD_VITA_PID);
        if (h)
        {
            log_line("Vita opened by ID although the device list didn't show it");
            dev_.vid = VD_VITA_VID;
            dev_.pid = VD_VITA_PID;
        }
    }
    if (list)
        sceUsbdFreeDeviceList(list);
    if (!h)
    {
        if (match)
        {
            last_error_ = rc;
            log_line("Vita found but sceUsbdOpen failed: %d (%s)", rc, err_name(rc));
        }
        return false;
    }

    // Raw configuration descriptor: header first, then the full length.
    static uint8_t config[8192];
    int got = vd_usbd_err(sceUsbdGetDescriptor(h, USB_DT_CONFIG, 0, config, 9));
    int total = got >= 4 ? (config[2] | (config[3] << 8)) : 0;
    if (total > (int)sizeof(config))
        total = sizeof(config);
    if (total >= 9)
        got = vd_usbd_err(sceUsbdGetDescriptor(h, USB_DT_CONFIG, 0, config, total));
    if (got < 9)
    {
        log_line("reading the Vita's configuration descriptor failed: %d (%s)", got,
                 err_name(got));
        sceUsbdClose(h);
        last_error_ = got < 0 ? got : VD_USB_ERR_IO;
        return false;
    }
    snprintf(dev_.path, sizeof(dev_.path), "usbd");
    if (vd_parse_vita_config(config, (unsigned)got, &dev_) != 0)
    {
        log_line("054c:1338 found, but it has no video interface (%d descriptor bytes). Is "
                 "the stream turned on?",
                 got);
        sceUsbdClose(h);
        return false;
    }
    log_line("Vita: video iface %u ep 0x%02x, %u format(s); audio %s (iface %u alt %u ep "
             "0x%02x max %u); pad iface %s",
             dev_.uvc.interface_number, dev_.video_in_count ? dev_.video_in_eps[0] : 0,
             dev_.uvc.format_count, dev_.audio_isoc_ep ? "yes" : "no",
             dev_.audio_stream_interface, dev_.audio_isoc_alt, dev_.audio_isoc_ep,
             dev_.audio_isoc_maxpkt, dev_.has_pad ? "yes" : "no (EP0 vendor requests)");
    for (unsigned fi = 0; fi < dev_.uvc.format_count; ++fi)
    {
        const VdUvcFormat &f = dev_.uvc.formats[fi];
        for (unsigned i = 0; i < f.frame_count; ++i)
            log_line("  format %u frame %u: %ux%u %s", f.format_index, f.frames[i].frame_index,
                     f.frames[i].width, f.frames[i].height, f.is_nv12 ? "NV12" : "other");
    }

    std::lock_guard<std::mutex> lock(ctrl_mutex_);
    handle_ = h;
    return true;
}

bool VitaLink::still_present()
{
    if (backend_ == Backend::ugen)
    {
        VdUsbVitaDevice d;
        return vd_usb_scan_vita(&d, 1) > 0;
    }
    libusb_device **list = nullptr;
    const int32_t n = sceUsbdGetDeviceList(&list);
    if (n < 0 || !list)
        return false;
    bool found = false;
    for (int32_t i = 0; i < n && !found; ++i)
    {
        VdUsbDeviceDescriptor d;
        if (sceUsbdGetDeviceDescriptor(list[i], &d) >= 0 && d.idVendor == VD_VITA_VID &&
            d.idProduct == VD_VITA_PID)
            found = true;
    }
    sceUsbdFreeDeviceList(list);
    return found;
}

void VitaLink::detach_and_claim(int iface, bool claim)
{
    const int active = vd_usbd_err(sceUsbdKernelDriverActive(handle_, iface));
    if (active == 1)
    {
        const int rc = vd_usbd_err(sceUsbdDetachKernelDriver(handle_, iface));
        log_line("interface %d had a system driver attached; detach -> %d (%s)", iface, rc,
                 err_name(rc));
    }
    if (claim)
    {
        const int rc = vd_usbd_err(sceUsbdClaimInterface(handle_, iface));
        if (rc < 0)
            log_line("claim interface %d -> %d (%s)", iface, rc, err_name(rc));
    }
}

bool VitaLink::start_video()
{
    if (backend_ == Backend::ugen)
        return start_video_ugen();
    if (!handle_)
        return false;
    const uint8_t vs = dev_.uvc.interface_number;
    bulk_ep_ = dev_.video_in_count ? dev_.video_in_eps[0] : 0x81;
    if (dev_.has_video_control)
        detach_and_claim(dev_.video_control_interface, false);
    detach_and_claim(vs, true);
    video_claimed_ = true;

    uint8_t format_index = 1;
    const VdUvcFrame *frame = nullptr;
    uint32_t frame_size = VD_NV12_FRAME_SIZE(960, 544);
    uint32_t interval = 166666;
    uint8_t frame_index = 1;
    if (vd_pick_frame(&dev_, want_w(), want_h(), &format_index, &frame) == 0 && frame)
    {
        frame_index = frame->frame_index;
        frame_size = VD_NV12_FRAME_SIZE(frame->width, frame->height);
        if (frame->default_interval)
            interval = frame->default_interval;
    }
    preferred_frame_ = frame_index;
    const uint32_t largest = vd_largest_nv12_frame(&dev_);
    max_payload_ = (largest > frame_size ? largest : frame_size) + UVC_PAYLOAD_HEADER_SIZE;

    // UVC probe/commit. On the Vita the SETUP of the commit is what starts
    // the stream (EP0 data stages are not delivered on its firmware, see
    // docs/VITA_INPUT_API.md section 7), so failures here are logged, not fatal.
    VdUvcStreamingControl probe;
    vd_uvc_default_probe(&probe, format_index, frame_index, interval, frame_size,
                         frame_size + UVC_PAYLOAD_HEADER_SIZE);
    static_assert(sizeof(VdUvcStreamingControl) == 34, "UVC 1.1 probe is 34 bytes");
    int rc = vd_usbd_err(sceUsbdControlTransfer(handle_, 0x21, UVC_SET_CUR,
                                                UVC_VS_PROBE_CONTROL << 8, vs,
                                                (unsigned char *)&probe, sizeof(probe),
                                                kCtrlTimeoutMs));
    log_line("UVC probe SET_CUR (format %u frame %u interval %u) -> %d", format_index,
             frame_index, interval, rc);
    VdUvcStreamingControl negotiated = probe;
    VdUvcStreamingControl readback;
    memset(&readback, 0, sizeof(readback));
    rc = vd_usbd_err(sceUsbdControlTransfer(handle_, 0xA1, UVC_GET_CUR,
                                            UVC_VS_PROBE_CONTROL << 8, vs,
                                            (unsigned char *)&readback, sizeof(readback),
                                            kCtrlTimeoutMs));
    if (rc >= 26 && readback.bFrameIndex != 0)
        negotiated = readback;
    log_line("UVC probe GET_CUR -> %d (frame %u)", rc, readback.bFrameIndex);
    rc = vd_usbd_err(sceUsbdControlTransfer(handle_, 0x21, UVC_SET_CUR,
                                            UVC_VS_COMMIT_CONTROL << 8, vs,
                                            (unsigned char *)&negotiated, sizeof(negotiated),
                                            kCtrlTimeoutMs));
    log_line("UVC commit SET_CUR -> %d (%s)", rc, err_name(rc));
    last_error_ = rc < 0 ? rc : 0;
    return true;
}

void VitaLink::stop_video()
{
    if (backend_ == Backend::ugen)
    {
        if (ugen_ && ugen_->uvc_open)
        {
            vd_uvc_session_close(&ugen_->uvc);
            ugen_->uvc_open = false;
        }
        return;
    }
    if (!handle_ || !video_claimed_)
        return;
    const uint8_t vs = dev_.uvc.interface_number;
    // Alt 0 on the streaming interface stops the Vita's stream (vita-udcd-uvc).
    sceUsbdSetInterfaceAltSetting(handle_, vs, 0);
    sceUsbdReleaseInterface(handle_, vs);
    video_claimed_ = false;
}

uint32_t VitaLink::frame_buffer_bytes() const
{
    const uint32_t base = max_payload_ ? max_payload_
                                       : VD_NV12_FRAME_SIZE(1280, 720) + UVC_PAYLOAD_HEADER_SIZE;
    return (base + kChunk + kMaxPacket - 1) / kMaxPacket * kMaxPacket;
}

FrameResult VitaLink::read_frame(uint8_t *buf, uint32_t cap, const uint8_t **nv12, int *w,
                                 int *h)
{
    if (backend_ == Backend::ugen)
        return read_frame_ugen(buf, cap, nv12, w, h);
    // The Vita sends each frame as one bulk request: a 12-byte UVC header and
    // the whole NV12 frame, ending in a short packet. Read in bounded chunks
    // (each a whole number of packets) until a chunk comes back short: that
    // marks the end of the frame. A frame joined part-way through fails the
    // size check and the next read starts on a clean boundary.
    cap = cap / kMaxPacket * kMaxPacket;
    uint32_t total = 0;
    int drains = 0;
    double t_first = 0;       // end of the first chunk (the frame is flowing)
    uint32_t first_bytes = 0; // its size
    for (;;)
    {
        uint32_t want = cap - total;
        if (want > kChunk)
            want = kChunk;
        if (want == 0)
        {
            // Larger than any advertised frame: drain to the next boundary.
            static uint8_t sink[kChunk];
            int32_t got = 0;
            int rc = vd_usbd_err(
                sceUsbdBulkTransfer(handle_, bulk_ep_, sink, kChunk, &got, kBulkTimeoutMs));
            if (rc < 0 || got < (int32_t)kChunk || ++drains > 64)
                return FrameResult::bad;
            continue;
        }
        int32_t got = 0;
        const int rc = vd_usbd_err(
            sceUsbdBulkTransfer(handle_, bulk_ep_, buf + total, (int32_t)want, &got,
                                kBulkTimeoutMs));
        if (got > 0)
        {
            if (total == 0)
            {
                t_first = now_seconds();
                first_bytes = (uint32_t)got;
            }
            total += (uint32_t)got;
        }
        if (rc == VD_USB_ERR_TIMEOUT)
            return total == 0 ? FrameResult::idle : FrameResult::bad;
        if (rc == VD_USB_ERR_PIPE)
        {
            sceUsbdClearHalt(handle_, bulk_ep_);
            last_error_ = rc;
            return FrameResult::bad;
        }
        if (rc < 0)
        {
            last_error_ = rc;
            return FrameResult::error;
        }
        if ((uint32_t)got < want)
            break; // short packet: end of this frame
    }

    if (total < UVC_PAYLOAD_HEADER_SIZE)
        return FrameResult::bad;
    const uint8_t header_len = buf[0];
    const uint8_t info = buf[1];
    if (header_len < 2 || header_len > 12 || !(info & UVC_STREAM_EOH) ||
        (info & UVC_STREAM_ERR))
        return FrameResult::bad;
    uint16_t fw = 0, fh = 0;
    if (!vd_match_nv12_frame(&dev_, total - header_len, preferred_frame_, &fw, &fh))
        return FrameResult::bad;
    *nv12 = buf + header_len;
    *w = fw;
    *h = fh;
    // Wire time of the whole frame: the rate over every chunk after the first
    // (the first one's start is hidden behind the wait for the frame).
    const double span = now_seconds() - t_first;
    if (total > first_bytes && span > 0)
        last_transfer_ms_ = (float)(span * 1000.0 * total / (double)(total - first_bytes));
    return FrameResult::ok;
}

int VitaLink::ctrl_chunks(uint8_t base_request, const uint8_t *data, unsigned chunks)
{
    std::lock_guard<std::mutex> lock(ctrl_mutex_);
    if (backend_ == Backend::ugen)
    {
        if (!ugen_ || ugen_->ctrl_fd < 0)
            return VD_USB_ERR_NO_DEVICE;
        for (unsigned i = 0; i < chunks; ++i)
        {
            const uint16_t wValue = (uint16_t)(data[i * 4] | (data[i * 4 + 1] << 8));
            const uint16_t wIndex = (uint16_t)(data[i * 4 + 2] | (data[i * 4 + 3] << 8));
            const int rc = usb_ctrl_xfer(ugen_->ctrl_fd, 0x40, (uint8_t)(base_request + i), wValue,
                                         wIndex, nullptr, 0, 0);
            if (rc != 0)
                return VD_USB_ERR_IO;
        }
        return 0;
    }
    if (!handle_)
        return VD_USB_ERR_NO_DEVICE;
    for (unsigned i = 0; i < chunks; ++i)
    {
        const uint16_t wValue = (uint16_t)(data[i * 4] | (data[i * 4 + 1] << 8));
        const uint16_t wIndex = (uint16_t)(data[i * 4 + 2] | (data[i * 4 + 3] << 8));
        const int rc = vd_usbd_err(sceUsbdControlTransfer(handle_, 0x40,
                                                          (uint8_t)(base_request + i), wValue,
                                                          wIndex, nullptr, 0, kInputTimeoutMs));
        if (rc < 0)
            return rc;
    }
    return 0;
}

int VitaLink::send_pad(const VdPadReport &report)
{
    if (backend_ == Backend::ugen)
    {
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        if (!ugen_ || ugen_->ctrl_fd < 0)
            return VD_USB_ERR_NO_DEVICE;
        // The PS5 build's hardware-proven sender (7 SETUP-only control OUTs).
        return usb_send_pad_report(ugen_->ctrl_fd, nullptr, &report) == 0 ? 0 : VD_USB_ERR_IO;
    }
    uint8_t wire[VD_PAD_WIRE_BYTES];
    if (vd_pad_serialize(&report, wire, sizeof(wire)) != VD_PAD_WIRE_BYTES)
        return VD_USB_ERR_INVALID;
    return ctrl_chunks(0x50, wire, 7);
}

int VitaLink::send_touch(const VdTouchReport &touch)
{
    if (backend_ == Backend::ugen)
    {
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        if (!ugen_ || ugen_->ctrl_fd < 0)
            return VD_USB_ERR_NO_DEVICE;
        // VITA5's core sender (4 SETUP-only control OUTs, bRequest 0x58..0x5B).
        return usb_send_touch_report(ugen_->ctrl_fd, &touch) == 0 ? 0 : VD_USB_ERR_IO;
    }
    uint8_t wire[VD_TOUCH_WIRE_BYTES];
    if (vd_touch_serialize(&touch, wire) != VD_TOUCH_WIRE_BYTES)
        return VD_USB_ERR_INVALID;
    return ctrl_chunks(0x58, wire, 4);
}

void VitaLink::recommit()
{
    if (backend_ == Backend::ugen)
    {
        if (ugen_ && ugen_->uvc_open)
            vd_uvc_recommit(&ugen_->uvc);
        return;
    }
    if (!handle_ || !video_claimed_)
        return;
    VdUvcStreamingControl probe;
    uint8_t format_index = 1;
    const VdUvcFrame *frame = nullptr;
    uint32_t frame_size = VD_NV12_FRAME_SIZE(960, 544), interval = 166666;
    uint8_t frame_index = preferred_frame_;
    if (vd_pick_frame(&dev_, want_w(), want_h(), &format_index, &frame) == 0 && frame)
    {
        frame_size = VD_NV12_FRAME_SIZE(frame->width, frame->height);
        if (frame->default_interval)
            interval = frame->default_interval;
    }
    vd_uvc_default_probe(&probe, format_index, frame_index, interval, frame_size,
                         frame_size + UVC_PAYLOAD_HEADER_SIZE);
    const uint8_t vs = dev_.uvc.interface_number;
    sceUsbdControlTransfer(handle_, 0x21, UVC_SET_CUR, UVC_VS_PROBE_CONTROL << 8, vs,
                           (unsigned char *)&probe, sizeof(probe), kCtrlTimeoutMs);
    sceUsbdControlTransfer(handle_, 0x21, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL << 8, vs,
                           (unsigned char *)&probe, sizeof(probe), kCtrlTimeoutMs);
}

// ---- audio -----------------------------------------------------------------

static VitaLink *g_audio_owner = nullptr;
static VdXferLayout g_layout;

void VitaLink::iso_callback(libusb_transfer *t)
{
    VitaLink *self = static_cast<VitaLink *>(vd_xfer_user_data(&g_layout, t));
    if (!self)
        self = g_audio_owner;
    if (self)
        self->on_iso(t);
}

void VitaLink::on_iso(libusb_transfer *t)
{
    const int status = vd_xfer_status(&layout_, t);
    if (status == VD_XFER_COMPLETED)
    {
        uint8_t *base = vd_xfer_buffer(&layout_, t);
        const int packets = vd_xfer_num_iso(&layout_, t);
        for (int i = 0; i < packets && i < kIsoPackets; ++i)
        {
            if (vd_xfer_iso_status(&layout_, t, i) != VD_XFER_COMPLETED)
                continue;
            unsigned n = vd_xfer_iso_actual(&layout_, t, i);
            if (n > iso_pkt_)
                n = iso_pkt_;
            n &= ~3u; // whole stereo int16 frames
            if (n && ring_)
            {
                ring_->push(reinterpret_cast<const int16_t *>(base + (size_t)i * iso_pkt_),
                            n / 4);
                audio_bytes_.fetch_add(n);
            }
        }
    }
    else if (status != VD_XFER_CANCELLED)
    {
        iso_errors_.fetch_add(1);
    }

    const bool fatal = status == VD_XFER_CANCELLED || status == VD_XFER_NO_DEVICE;
    if (!audio_stopping_.load() && !fatal && iso_errors_.load() < 200)
    {
        if (sceUsbdSubmitTransfer(t) >= 0)
            return;
    }
    // The last transfer ending on its own (errors, unplug) means audio has
    // stopped: report it so the link thread can tear down and retry.
    if (inflight_.fetch_sub(1) == 1 && !audio_stopping_.load())
        audio_running_ = false;
}

void VitaLink::event_loop()
{
    while (!events_stop_.load())
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        sceUsbdHandleEventsTimeout(&tv);
    }
}

bool VitaLink::start_audio(AudioRing *ring)
{
    if (backend_ == Backend::ugen)
        return start_audio_ugen(ring);
    if (!handle_ || audio_running_.load())
        return audio_running_.load();
    if (events_.joinable())
    {
        log_line("audio: stream stopped (%u transfer errors) - restarting", iso_errors_.load());
        stop_audio(); // a previous session ended on its own: clean it up first
    }
    audio_problem_ = "";
    if (!dev_.audio_isoc_ep)
    {
        audio_problem_ = "This Vita stream has no USB audio (needs the VitaUSBStream build)";
        log_line("audio: %s", audio_problem_);
        return false;
    }
    const uint16_t mp = dev_.audio_isoc_maxpkt;
    iso_pkt_ = (unsigned)(mp & 0x7FF) * (1u + ((mp >> 11) & 3u));
    if (iso_pkt_ == 0)
        iso_pkt_ = 192;

    // Learn the transfer struct layout from a fresh allocation.
    libusb_transfer *probe = sceUsbdAllocTransfer(kIsoPackets);
    if (!probe)
    {
        audio_problem_ = "Out of memory for audio transfers";
        return false;
    }
    if (vd_xfer_layout_detect(probe, kIsoPackets, &layout_) == VD_XFER_LAYOUT_NONE)
    {
        sceUsbdFreeTransfer(probe);
        audio_problem_ = "Audio isn't supported by this firmware's USB library";
        log_line("audio: unknown libusb_transfer layout - audio disabled");
        return false;
    }
    log_line("audio: transfer layout %s, packet %u bytes",
             layout_.kind == VD_XFER_LAYOUT_UPSTREAM ? "libusb" : "FreeBSD", iso_pkt_);
    g_layout = layout_;
    g_audio_owner = this;

    // Interface setup: the PS4 may have its own USB-audio driver on these.
    const int as = dev_.audio_stream_interface;
    if (dev_.has_audio && dev_.uac.interface_number != as)
        detach_and_claim(dev_.uac.interface_number, false);
    detach_and_claim(as, true);
    audio_claimed_ = true;
    int rc = vd_usbd_err(sceUsbdSetInterfaceAltSetting(handle_, as, dev_.audio_isoc_alt));
    log_line("audio: interface %d alt %u -> %d (%s)", as, dev_.audio_isoc_alt, rc, err_name(rc));
    if (rc < 0)
    {
        sceUsbdFreeTransfer(probe);
        sceUsbdReleaseInterface(handle_, as);
        audio_claimed_ = false;
        audio_problem_ = "The PS4 wouldn't switch on the Vita's audio interface";
        return false;
    }

    ring_ = ring;
    ring->reset();
    audio_stopping_ = false;
    iso_errors_ = 0;
    audio_bytes_ = 0;
    inflight_ = 0;
    events_stop_ = false;
    iso_[0] = probe;
    for (int i = 0; i < kIsoTransfers; ++i)
    {
        if (!iso_[i])
            iso_[i] = sceUsbdAllocTransfer(kIsoPackets);
        iso_buf_[i] = (uint8_t *)calloc(kIsoPackets, iso_pkt_);
    }
    events_ = std::thread(&VitaLink::event_loop, this);

    int submitted = 0;
    for (int i = 0; i < kIsoTransfers; ++i)
    {
        if (!iso_[i] || !iso_buf_[i])
            continue;
        vd_xfer_fill_iso(&layout_, iso_[i], handle_, dev_.audio_isoc_ep, iso_buf_[i],
                         kIsoPackets, iso_pkt_, (void *)&VitaLink::iso_callback, this, 0);
        inflight_.fetch_add(1);
        rc = vd_usbd_err(sceUsbdSubmitTransfer(iso_[i]));
        if (rc < 0)
        {
            inflight_.fetch_sub(1);
            log_line("audio: submit transfer %d -> %d (%s)", i, rc, err_name(rc));
        }
        else
        {
            ++submitted;
        }
    }
    if (submitted == 0)
    {
        audio_problem_ = "The PS4 refused the audio transfers";
        stop_audio();
        return false;
    }
    audio_running_ = true;
    log_line("audio: streaming (%d transfers x %d packets)", submitted, kIsoPackets);
    return true;
}

void VitaLink::stop_audio()
{
    if (backend_ == Backend::ugen)
    {
        stop_audio_ugen();
        return;
    }
    audio_stopping_ = true;
    for (int i = 0; i < kIsoTransfers; ++i)
        if (iso_[i])
            sceUsbdCancelTransfer(iso_[i]);
    const double deadline = now_seconds() + 1.5;
    while (inflight_.load() > 0 && now_seconds() < deadline)
    {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, nullptr);
    }
    events_stop_ = true;
    if (events_.joinable())
        events_.join();
    const bool safe_to_free = inflight_.load() <= 0;
    if (!safe_to_free)
        log_line("audio: %d transfer(s) did not finish; keeping their buffers", inflight_.load());
    for (int i = 0; i < kIsoTransfers; ++i)
    {
        if (safe_to_free)
        {
            if (iso_[i])
                sceUsbdFreeTransfer(iso_[i]);
            free(iso_buf_[i]);
        }
        iso_[i] = nullptr;
        iso_buf_[i] = nullptr;
    }
    if (handle_ && audio_claimed_)
    {
        const int as = dev_.audio_stream_interface;
        sceUsbdSetInterfaceAltSetting(handle_, as, 0);
        sceUsbdReleaseInterface(handle_, as);
    }
    audio_claimed_ = false;
    audio_running_ = false;
    ring_ = nullptr;
}

void VitaLink::close()
{
    if (backend_ == Backend::ugen)
    {
        close_ugen();
        backend_ = Backend::none;
        return;
    }
    if (audio_running_.load() || events_.joinable())
        stop_audio();
    stop_video();
    std::lock_guard<std::mutex> lock(ctrl_mutex_);
    if (handle_)
    {
        sceUsbdClose(handle_);
        handle_ = nullptr;
    }
    backend_ = Backend::none;
}

// ---- raw /dev/ugen backend (GoldHEN) ----------------------------------------------

bool VitaLink::find_ugen()
{
    VdUsbVitaDevice d;
    memset(&d, 0, sizeof(d));
    if (vd_usb_scan_vita(&d, 1) == 0)
    {
        // Diagnostics: what is on the raw bus?
        char scan[512];
        int sp = snprintf(scan, sizeof(scan), "ugen:");
        DIR *dir = opendir("/dev");
        int nodes = 0;
        if (dir)
        {
            struct dirent *e;
            while ((e = readdir(dir)))
            {
                unsigned bus = 0, addr = 0;
                char extra = 0;
                if (sscanf(e->d_name, "ugen%u.%u%c", &bus, &addr, &extra) != 2)
                    continue;
                ++nodes;
                char path[48];
                snprintf(path, sizeof(path), "/dev/%s", e->d_name);
                const int fd = open(path, O_RDWR | O_NONBLOCK);
                if (fd < 0)
                {
                    sp += snprintf(scan + sp, sizeof(scan) - (size_t)sp, " %s(open errno %d)",
                                   e->d_name, errno);
                    continue;
                }
                struct usb_device_descriptor dd;
                memset(&dd, 0, sizeof(dd));
                if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0)
                    sp += snprintf(scan + sp, sizeof(scan) - (size_t)sp, " %s=%04x:%04x/%02x",
                                   e->d_name, UGETW(dd.idVendor), UGETW(dd.idProduct),
                                   dd.bDeviceClass);
                else
                    sp += snprintf(scan + sp, sizeof(scan) - (size_t)sp, " %s(desc errno %d)",
                                   e->d_name, errno);
                ::close(fd);
                if (sp > (int)sizeof(scan) - 48)
                    break;
            }
            closedir(dir);
        }
        if (nodes == 0)
            snprintf(scan + sp, sizeof(scan) - (size_t)sp, " no ugen nodes");
        static char last[512];
        if (strcmp(scan, last) != 0 || scans_ % 30 == 1)
        {
            log_line("scan %u: %s", scans_, scan);
            snprintf(last, sizeof(last), "%s", scan);
        }
        return false;
    }
    ugen_ = new Ugen();
    ugen_->dev = d;
    dev_ = d;
    ugen_->ctrl_fd = open(d.path, O_RDWR | O_NONBLOCK);
    log_line("Vita on %s: video iface %u ep 0x%02x, audio ep 0x%02x (iface %u alt %u), ctrl fd %d",
             d.path, d.uvc.interface_number, d.video_in_count ? d.video_in_eps[0] : 0,
             d.audio_isoc_ep, d.audio_stream_interface, d.audio_isoc_alt, ugen_->ctrl_fd);
    return true;
}

bool VitaLink::start_video_ugen()
{
    if (!ugen_)
        return false;
    if (ugen_->uvc_open)
    {
        vd_uvc_session_close(&ugen_->uvc);
        ugen_->uvc_open = false;
    }
    if (vd_uvc_session_open(&ugen_->uvc, &ugen_->dev) != 0)
    {
        log_line("ugen: video session open failed (errno %d)", errno);
        last_error_ = VD_USB_ERR_IO;
        return false;
    }
    ugen_->uvc_open = true;
    uint8_t format_index = 1;
    const VdUvcFrame *frame = nullptr;
    uint8_t frame_index = 1;
    uint32_t interval = 166666, frame_size = VD_NV12_FRAME_SIZE(960, 544);
    if (vd_pick_frame(&ugen_->dev, want_w(), want_h(), &format_index, &frame) == 0 && frame)
    {
        frame_index = frame->frame_index;
        frame_size = VD_NV12_FRAME_SIZE(frame->width, frame->height);
        if (frame->default_interval)
            interval = frame->default_interval;
    }
    preferred_frame_ = frame_index;
    const int rc = vd_uvc_start_stream(&ugen_->uvc, format_index, frame_index, interval,
                                       frame_size);
    log_line("ugen: start stream (format %u frame %u) -> %d", format_index, frame_index, rc);
    if (rc != 0)
    {
        // vd_uvc_start_stream closes the session itself on failure.
        ugen_->uvc_open = false;
        last_error_ = VD_USB_ERR_IO;
        return false;
    }
    last_error_ = 0;
    return true;
}

FrameResult VitaLink::read_frame_ugen(uint8_t *buf, uint32_t cap, const uint8_t **nv12, int *w,
                                      int *h)
{
    if (!ugen_ || !ugen_->uvc_open)
    {
        last_error_ = VD_USB_ERR_NO_DEVICE;
        return FrameResult::error;
    }
    int done = 0, fid = 0;
    const int n = vd_uvc_read_payload(&ugen_->uvc, buf, cap, &done, &fid);
    // Since VITA5 beta 2026-10-07 the reader preserves the transport errno:
    // -ETIMEDOUT = idle (no frame yet), -ENXIO/-ENODEV = the Vita left the
    // bus. -3/-4 are its own header checks (bad payload) and -1 a bad call.
    if (n == -ETIMEDOUT)
        return FrameResult::idle;
    if (n == -3 || n == -4)
        return FrameResult::bad;
    if (n < 0)
    {
        last_error_ = (n == -ENXIO || n == -ENODEV) ? VD_USB_ERR_NO_DEVICE : VD_USB_ERR_IO;
        return FrameResult::error;
    }
    uint16_t fw = 0, fh = 0;
    if (!vd_match_nv12_frame(&ugen_->dev, (uint32_t)n, preferred_frame_, &fw, &fh))
        return FrameResult::bad;
    *nv12 = buf + buf[0];
    *w = fw;
    *h = fh;
    return FrameResult::ok;
}

bool VitaLink::start_audio_ugen(AudioRing *ring)
{
    if (!ugen_ || audio_running_.load())
        return audio_running_.load();
    audio_problem_ = "";
    if (!ugen_->dev.audio_isoc_ep)
    {
        audio_problem_ = "This Vita stream has no USB audio (needs the VitaUSBStream build)";
        return false;
    }
    if (ugen_->uac_thread.joinable())
        stop_audio_ugen();
    if (vd_uac_session_open(&ugen_->uac, &ugen_->dev) != 0)
    {
        audio_problem_ = "Couldn't open the Vita's audio interface";
        log_line("ugen: audio session open failed (errno %d)", errno);
        return false;
    }
    ugen_->uac_open = true;
    const int rc = vd_uac_start(&ugen_->uac, 0);
    log_line("ugen: audio start -> %d", rc);
    if (rc != 0)
    {
        vd_uac_session_close(&ugen_->uac);
        ugen_->uac_open = false;
        audio_problem_ = "The PS4 refused the audio stream";
        return false;
    }
    ring_ = ring;
    ring->reset();
    audio_bytes_ = 0;
    ugen_->uac_stop = false;
    audio_running_ = true;
    Ugen *u = ugen_;
    ugen_->uac_thread = std::thread([this, u] {
        static uint8_t pcm[VD_UAC_MAX_FRAMES * 1024];
        int errors = 0;
        while (!u->uac_stop.load())
        {
            uint32_t len = 0;
            const int rc = vd_uac_read(&u->uac, pcm, sizeof(pcm), &len, nullptr, 0, nullptr);
            if (rc == 0)
            {
                errors = 0;
                len &= ~3u;
                if (len && ring_)
                {
                    ring_->push(reinterpret_cast<const int16_t *>(pcm), len / 4);
                    audio_bytes_.fetch_add(len);
                }
            }
            else if (++errors > 50)
            {
                log_line("ugen: audio stopped after repeated read errors (%d)", rc);
                break;
            }
        }
        audio_running_ = false;
    });
    return true;
}

void VitaLink::stop_audio_ugen()
{
    if (!ugen_)
        return;
    ugen_->uac_stop = true;
    if (ugen_->uac_thread.joinable())
        ugen_->uac_thread.join();
    if (ugen_->uac_open)
    {
        vd_uac_session_close(&ugen_->uac);
        ugen_->uac_open = false;
    }
    audio_running_ = false;
    ring_ = nullptr;
}

void VitaLink::close_ugen()
{
    if (!ugen_)
        return;
    stop_audio_ugen();
    if (ugen_->uvc_open)
    {
        vd_uvc_session_close(&ugen_->uvc);
        ugen_->uvc_open = false;
    }
    std::lock_guard<std::mutex> lock(ctrl_mutex_);
    if (ugen_->ctrl_fd >= 0)
        ::close(ugen_->ctrl_fd);
    delete ugen_;
    ugen_ = nullptr;
}

} // namespace vd4
