// VITA4 - single-producer / single-consumer PCM ring (stereo int16 frames).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Producer: the USB isochronous completion callback. Consumer: the audio
// output thread. Lock-free; when full the producer drops the incoming audio
// (the consumer trims latency by skipping ahead, see trim()). Portable.
#pragma once
#include <stdint.h>
#include <string.h>

#include <atomic>

namespace vd4
{

class AudioRing
{
  public:
    static constexpr uint32_t kFrames = 16384; // ~340 ms at 48 kHz (power of two)

    void reset()
    {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    uint32_t available() const
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    // Push interleaved stereo frames (4 bytes each). Returns frames accepted.
    uint32_t push(const int16_t *frames, uint32_t count)
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t space = kFrames - (head - tail);
        if (count > space)
            count = space;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t idx = (head + i) & (kFrames - 1);
            data_[idx * 2] = frames[i * 2];
            data_[idx * 2 + 1] = frames[i * 2 + 1];
        }
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    // Pop up to `count` frames. Returns frames written to `out`.
    uint32_t pop(int16_t *out, uint32_t count)
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t avail = head - tail;
        if (count > avail)
            count = avail;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t idx = (tail + i) & (kFrames - 1);
            out[i * 2] = data_[idx * 2];
            out[i * 2 + 1] = data_[idx * 2 + 1];
        }
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // Consumer-side latency cap: drop the oldest audio beyond `keep` frames.
    uint32_t trim(uint32_t keep)
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t avail = head - tail;
        if (avail <= keep)
            return 0;
        tail_.store(head - keep, std::memory_order_release);
        return avail - keep;
    }

  private:
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    int16_t data_[kFrames * 2];
};

} // namespace vd4
