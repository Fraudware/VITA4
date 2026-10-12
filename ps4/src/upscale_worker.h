// VITA4 - runs the CPU upscaler (upscale.h) off the UI thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI loop must keep presenting every vblank (menu, toasts, the controller
// all feel it otherwise), but an upscaled frame takes longer than a vblank on
// the PS4's CPU. So the UI hands each new Vita frame to this worker (newest
// wins; frames that arrive while it is busy are skipped), the worker upscales
// it with its own row pool, and the UI shows the newest finished picture.
#pragma once
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <pthread.h>

#include "display.h"
#include "upscale.h"

namespace vd4
{

class UpscaleWorker
{
  public:
    static constexpr int kOutW = 1920, kOutH = 1080;

    // True for sources whose 2x covers the screen (the Vita's 960x544).
    static bool supported(int w, int h)
    {
        return w * 2 == kOutW && h * 2 >= kOutH && h * 2 <= kOutH + 16;
    }

    UpscaleWorker() = default;
    UpscaleWorker(const UpscaleWorker &) = delete;
    UpscaleWorker &operator=(const UpscaleWorker &) = delete;
    ~UpscaleWorker();

    // Allocates everything (about 41 MB) and starts the worker thread with
    // `helpers` extra row threads. On failure everything is released again
    // and false is returned. Restartable after stop().
    // `first_core`: the worker runs on that game core, helper i on first_core+1+i.
    bool start(int helpers, int first_core = -1);
    void stop(); // joins the threads and frees all memory
    bool running() const
    {
        return started_;
    }

    // UI thread: offer the newest frame (copied; returns at once).
    // `sharpness` 0 (off) .. 4 (max): FSR 2's slider in quarters, denoise on.
    void submit(const uint8_t *nv12, int w, int h, int mode, int sharpness = 4);
    // UI thread: the newest finished picture (kOutW x kOutH, 0xFFRRGGBB) made
    // since the last reset(), or nullptr. Stays valid until the next call.
    // `seq` (optional) gets the picture's number, new for every finished frame.
    const uint32_t *latest(uint64_t *seq = nullptr);
    // UI thread: forget every picture (mode change to Off, new stream).
    void reset();

    float fps() const
    {
        return fps_.load();
    }
    float ms() const
    {
        return ms_.load();
    }

  private:
    void run();
    static void *thread_main(void *self);

    pthread_t thread_{};
    bool started_ = false;
    RowPool *pool_ = nullptr;
    VdUpscaler *up_ = nullptr;

    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
    // Input: the pending frame (UI writes) and the one being worked on.
    uint8_t *in_[2] = {};
    int pending_ = 0;
    bool pending_ready_ = false;
    int in_w_ = 0, in_h_ = 0, in_mode_ = 0, in_sharp_ = 4;
    long in_gen_ = 0;
    // Output triple buffer: worker writes, `ready` is the newest, UI reads.
    uint32_t *out_[3] = {};
    int write_ = 0, ready_ = 1, read_ = 2;
    bool ready_new_ = false;
    long ready_gen_ = -1, read_gen_ = -1;
    uint64_t ready_seq_ = 0, read_seq_ = 0, next_seq_ = 0;
    long gen_ = 0;

    std::atomic<float> fps_{0}, ms_{0};
    int frames_done_ = 0; // worker thread only (reset by start)
    int core_ = -1;
};

} // namespace vd4
