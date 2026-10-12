// VITA4 - PS4 video output (two 1920x1080 32-bit scan-out buffers).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace vd4
{

class Display
{
  public:
    static constexpr int kWidth = 1920;
    static constexpr int kHeight = 1080;

    bool init();
    // The buffer to draw this frame (write-only video memory).
    uint32_t *back() const
    {
        return buffers_[current_];
    }
    int back_index() const
    {
        return current_;
    }
    // Queue the back buffer for the next vblank and wait until it is shown.
    void present();

    // TV-scaler test: two extra scan-out buffers at a smaller size (buffer
    // indices 2-3), which the PS4's display hardware scales to the screen.
    // Returns the sceVideoOutRegisterBuffers result (0 = accepted).
    int init_small(int w, int h);
    bool small_ready() const
    {
        return small_[0] != nullptr;
    }
    int small_width() const
    {
        return small_w_;
    }
    uint32_t *small_back() const
    {
        return small_[small_cur_];
    }
    // Shows small_back() (when `fresh`; otherwise keeps the small buffer on
    // screen) at the next vblank and waits for it.
    void present_small(bool fresh);

  private:
    void wait_flip(long id);

    int handle_ = -1;
    uint32_t *buffers_[2] = {};
    int current_ = 0;
    uint32_t *small_[2] = {};
    int small_w_ = 0, small_h_ = 0;
    int small_cur_ = 0;   // the one to draw next
    int small_shown_ = -1; // the one on screen (-1: none yet)
    long frame_ = 1;
    uint64_t queue_ = 0;
};

// Fixed pool of helper threads that split a frame's rows.
class RowPool
{
  public:
    typedef void (*Job)(void *ctx, int row_begin, int row_end, int worker);
    // first_core >= 0 pins helper i to game core first_core + i (see cpu.h).
    explicit RowPool(int helpers, int first_core = -1);
    ~RowPool();
    // Run job over [0, rows) split into (helpers + 1) bands; returns when done.
    void run(Job job, void *ctx, int rows);
    int workers() const
    {
        return helpers_ + 1;
    }

    struct Impl;

  private:
    static void *helper_main(void *arg);
    Impl *impl_;
    int helpers_;
};

} // namespace vd4
