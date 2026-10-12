// VITA4 - PS4 video output and the row-splitting render pool. See display.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"

#include <string.h>

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "log.h"

namespace vd4
{

bool Display::init()
{
    handle_ = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (handle_ < 0)
    {
        log_line("sceVideoOutOpen failed: 0x%08x", (unsigned)handle_);
        return false;
    }
    OrbisKernelEqueue q;
    if (sceKernelCreateEqueue(&q, "vita4 flip") < 0)
    {
        log_line("flip queue creation failed");
        return false;
    }
    sceVideoOutAddFlipEvent(q, handle_, nullptr);
    queue_ = q;

    const size_t fb_bytes = (size_t)kWidth * kHeight * 4;
    const size_t align = 0x200000;
    const size_t total = (fb_bytes * 2 + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3,
                                           &phys);
    if (rc < 0)
    {
        log_line("direct memory allocation failed: 0x%08x", (unsigned)rc);
        return false;
    }
    void *mem = nullptr;
    rc = sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align);
    if (rc < 0)
    {
        log_line("direct memory map failed: 0x%08x", (unsigned)rc);
        return false;
    }
    buffers_[0] = (uint32_t *)mem;
    buffers_[1] = (uint32_t *)((uint8_t *)mem + fb_bytes);
    for (auto *b : buffers_)
        for (size_t i = 0; i < (size_t)kWidth * kHeight; ++i)
            b[i] = 0xFF000000u;

    OrbisVideoOutBufferAttribute attr;
    // A8R8G8B8 sRGB, linear tiling - the format of the OpenOrbis samples.
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, kWidth, kHeight, kWidth);
    rc = sceVideoOutRegisterBuffers(handle_, 0, (void *const *)buffers_, 2, &attr);
    if (rc != 0)
    {
        log_line("sceVideoOutRegisterBuffers failed: 0x%08x", (unsigned)rc);
        return false;
    }
    sceVideoOutSetFlipRate(handle_, 0); // 60 Hz
    current_ = 0;
    return true;
}

int Display::init_small(int w, int h)
{
    const size_t bytes = (size_t)w * h * 4;
    const size_t align = 0x200000;
    const size_t total = (bytes * 2 + align - 1) / align * align;
    off_t phys = 0;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3,
                                           &phys);
    if (rc < 0)
    {
        log_line("TV scaler: direct memory allocation failed: 0x%08x", (unsigned)rc);
        return rc;
    }
    void *mem = nullptr;
    rc = sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, align);
    if (rc < 0)
    {
        log_line("TV scaler: direct memory map failed: 0x%08x", (unsigned)rc);
        return rc;
    }
    uint32_t *bufs[2] = {(uint32_t *)mem, (uint32_t *)((uint8_t *)mem + bytes)};
    for (auto *b : bufs)
        for (size_t i = 0; i < (size_t)w * h; ++i)
            b[i] = 0xFF000000u;
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, w, h, w);
    rc = sceVideoOutRegisterBuffers(handle_, 2, (void *const *)bufs, 2, &attr);
    log_line("TV scaler: registering %dx%d scan-out buffers -> 0x%08x", w, h, (unsigned)rc);
    if (rc != 0)
        return rc; // (the direct memory stays mapped; it is small and this is a one-off test)
    small_[0] = bufs[0];
    small_[1] = bufs[1];
    small_w_ = w;
    small_h_ = h;
    small_cur_ = 0;
    small_shown_ = -1;
    return 0;
}

void Display::present_small(bool fresh)
{
    if (fresh || small_shown_ < 0)
    {
        small_shown_ = small_cur_;
        small_cur_ ^= 1;
    }
    const long id = frame_++;
    sceVideoOutSubmitFlip(handle_, 2 + small_shown_, ORBIS_VIDEO_OUT_FLIP_VSYNC, id);
    wait_flip(id);
}

void Display::present()
{
    const long id = frame_++;
    sceVideoOutSubmitFlip(handle_, current_, ORBIS_VIDEO_OUT_FLIP_VSYNC, id);
    wait_flip(id);
    current_ ^= 1;
}

void Display::wait_flip(long id)
{
    OrbisKernelEqueue q = queue_;
    for (int guard = 0; guard < 8; ++guard)
    {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(handle_, &st);
        if (st.flipArg >= id)
            break;
        OrbisKernelEvent ev;
        int count = 0;
        OrbisKernelUseconds timeout = 100000;
        if (sceKernelWaitEqueue(q, &ev, 1, &count, &timeout) != 0)
            break;
    }
}

} // namespace vd4
