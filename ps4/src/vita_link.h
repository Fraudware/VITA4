// VITA4 - the USB link to the docked PS Vita, over the PS4's libSceUsbd.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the PS5 this work is split between a root payload (FreeBSD ugen) and the
// sandboxed app. The PS4 exposes a libusb-style host library to homebrew, so
// VITA4 is one app: it opens the Vita directly and speaks the same protocol:
//   video  UVC probe/commit on EP0, NV12 frames on bulk IN 0x81
//   audio  UAC isochronous IN on the AudioStreaming interface's alt 1
//   input  28-byte pad reports / 16-byte touch records as SETUP-only vendor
//          control transfers (bRequest 0x50.. / 0x58..), docs/VITA_INPUT_API.md
#pragma once
#include <stdint.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "audio_ring.h"
extern "C" {
#include "pad_passthrough.h"
}
#include "usbd.h"
#include "vita_desc.h"
#include "xfer_layout.h"

namespace vd4
{

enum class FrameResult
{
    ok,
    idle,    // no data within the timeout (Vita screen off, stream not started)
    bad,     // a payload arrived but was not a whole frame - dropped
    error,   // USB error (see last_error())
};

enum class Backend
{
    none,
    usbd, // the PS4's libSceUsbd (libusb-style)
    ugen, // raw FreeBSD /dev/ugen*, the PS5 build's proven core code (needs GoldHEN)
};

class VitaLink
{
  public:
    // Allow the /dev/ugen fallback (set once the GoldHEN jailbreak worked).
    void set_ugen_allowed(bool allowed)
    {
        ugen_allowed_ = allowed;
    }
    // 1.30: ask the Vita for its 480x272 picture (4x less to send over USB:
    // less lag, softer picture). Takes effect at the next (re)start.
    void set_fast_picture(bool fast)
    {
        fast_picture_ = fast;
    }
    Backend backend() const
    {
        return backend_;
    }

    // sceUsbdInit. Returns the raw result (0 = ok).
    int init();
    void shutdown();

    // Scan the bus for the Vita (054C:1338). On success the device is open
    // and its descriptors parsed. `seen` receives a short list of the
    // devices on the bus for the log.
    bool find_and_open(char *seen, int seen_cap);
    void close();
    bool is_open() const
    {
        return backend_ != Backend::none;
    }
    // Is the Vita still on the bus (device-list scan, no I/O on the handle)?
    bool still_present();

    bool start_video();
    void stop_video();
    // Read one frame payload into `buf`. On ok, *nv12 points at the NV12
    // data inside `buf` and *w/*h hold its size.
    FrameResult read_frame(uint8_t *buf, uint32_t cap, const uint8_t **nv12, int *w, int *h);
    uint32_t frame_buffer_bytes() const;

    bool start_audio(AudioRing *ring);
    void stop_audio();
    bool audio_running() const
    {
        return audio_running_.load();
    }
    const char *audio_problem() const
    {
        return audio_problem_;
    }

    // Input path. Thread-safe against close(). Return 0 or a libusb error.
    int send_pad(const VdPadReport &report);
    int send_touch(const VdTouchReport &touch);
    // Re-send the UVC probe/commit pair (VITA5 re-commits while the stream
    // is idle: the Vita's EP0 data stages are flaky). Cheap and safe.
    void recommit();

    const VdUsbVitaDevice &device() const
    {
        return dev_;
    }
    // Milliseconds the last whole frame took on the wire, from the end of its
    // first 64 KB chunk to its last byte, scaled up by one chunk (sceUsbd
    // path; 0 when not measured, e.g. on the raw /dev/ugen path).
    float last_transfer_ms() const
    {
        return last_transfer_ms_;
    }
    int last_error() const
    {
        return last_error_;
    }
    uint32_t audio_bytes() const
    {
        return audio_bytes_.load();
    }

  private:
    struct Ugen;
    bool find_usbd(char *seen, int seen_cap);
    bool find_ugen();
    bool start_video_ugen();
    FrameResult read_frame_ugen(uint8_t *buf, uint32_t cap, const uint8_t **nv12, int *w,
                                int *h);
    bool start_audio_ugen(AudioRing *ring);
    void stop_audio_ugen();
    void close_ugen();
    int ctrl_chunks(uint8_t base_request, const uint8_t *data, unsigned chunks);
    void detach_and_claim(int iface, bool claim);
    static void iso_callback(libusb_transfer *t);
    void on_iso(libusb_transfer *t);
    void event_loop();

    Backend backend_ = Backend::none;
    bool ugen_allowed_ = false;
    Ugen *ugen_ = nullptr;
    uint32_t scans_ = 0;
    char last_scan_[512] = "";
    libusb_device_handle *handle_ = nullptr;
    std::mutex ctrl_mutex_; // guards handle_ for the input thread
    VdUsbVitaDevice dev_{};
    bool video_claimed_ = false;
    uint8_t bulk_ep_ = 0x81;
    std::atomic<bool> fast_picture_{false};
    int want_w() const
    {
        return fast_picture_.load() ? 480 : 960;
    }
    int want_h() const
    {
        return fast_picture_.load() ? 272 : 544;
    }
    uint8_t preferred_frame_ = 1;
    uint32_t max_payload_ = 0;
    int last_error_ = 0;
    float last_transfer_ms_ = 0;

    // Audio.
    static constexpr int kIsoTransfers = 4;
    static constexpr int kIsoPackets = 32;
    VdXferLayout layout_{};
    libusb_transfer *iso_[kIsoTransfers] = {};
    uint8_t *iso_buf_[kIsoTransfers] = {};
    unsigned iso_pkt_ = 0;
    AudioRing *ring_ = nullptr;
    std::atomic<int> inflight_{0};
    std::atomic<bool> audio_stopping_{false};
    std::atomic<bool> audio_running_{false};
    std::atomic<bool> events_stop_{false};
    std::atomic<uint32_t> audio_bytes_{0};
    std::atomic<uint32_t> iso_errors_{0};
    bool audio_claimed_ = false;
    std::thread events_;
    const char *audio_problem_ = "";
};

} // namespace vd4
