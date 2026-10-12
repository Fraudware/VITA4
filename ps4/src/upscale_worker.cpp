// VITA4 - background upscaler. See upscale_worker.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "upscale_worker.h"

#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "log.h"

namespace vd4
{

namespace
{

constexpr size_t kMaxIn = (size_t)UpscaleWorker::kOutW / 2 * (UpscaleWorker::kOutH / 2 + 8) * 3 / 2;

struct ConvertJob
{
    VdUpscaler *u;
    const uint8_t *nv12;
};
void convert_rows(void *ctx, int begin, int end, int)
{
    ConvertJob *j = static_cast<ConvertJob *>(ctx);
    vd_upscale_convert(j->u, j->nv12, begin, end);
}
void analyze_rows(void *ctx, int begin, int end, int)
{
    vd_upscale_analyze(static_cast<VdUpscaler *>(ctx), begin, end);
}

struct RenderJob
{
    VdUpscaler *u;
    int mode;
    uint32_t *dst;
    int row_offset;
    std::atomic<int> failed{0};
    bool trace = false;
};
void render_rows(void *ctx, int begin, int end, int worker)
{
    RenderJob *j = static_cast<RenderJob *>(ctx);
    const double t0 = j->trace ? now_seconds() : 0;
    if (vd_upscale_render(j->u, j->mode, j->dst, UpscaleWorker::kOutW, j->row_offset,
                          UpscaleWorker::kOutH, begin, end) != 0)
        j->failed = 1;
    if (j->trace)
        log_line("upscaler:     band %d (rows %d-%d) on core %d: %.0f ms", worker, begin, end,
                 cpu_current(), (now_seconds() - t0) * 1000.0);
}

} // namespace

UpscaleWorker::~UpscaleWorker()
{
    stop();
}

void *UpscaleWorker::thread_main(void *self)
{
    static_cast<UpscaleWorker *>(self)->run();
    return nullptr;
}

bool UpscaleWorker::start(int helpers, int first_core)
{
    core_ = first_core;
    if (started_)
        return true;
    bool ok = true;
    for (auto &p : in_)
        ok = ok && (p = (uint8_t *)malloc(kMaxIn)) != nullptr;
    for (auto &p : out_)
    {
        ok = ok && (p = (uint32_t *)malloc((size_t)kOutW * kOutH * 4)) != nullptr;
        if (p)
            memset(p, 0, (size_t)kOutW * kOutH * 4); // touch it now: fail here, not mid-frame
    }
    log_line("upscaler: picture buffers %s", ok ? "ok" : "FAILED");
    // The Vita's size up front, so a memory shortage shows here, not mid-stream.
    ok = ok && (up_ = vd_upscaler_create(kOutW / 2, (kOutH + 8) / 2)) != nullptr;
    log_line("upscaler: work buffers %s", ok ? "ok" : "FAILED");
    if (!ok)
    {
        stop();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        quit_ = false;
        pending_ready_ = false;
        ready_new_ = false;
        ready_gen_ = read_gen_ = -1;
        ++gen_;
    }
    pool_ = new RowPool(helpers, first_core >= 0 ? first_core + 1 : -1);
    log_line("upscaler: %d row threads", pool_->workers());
    frames_done_ = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    const int rc = pthread_create(&thread_, &attr, &UpscaleWorker::thread_main, this);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        log_line("upscaler: worker thread not created (%d)", rc);
        stop();
        return false;
    }
    started_ = true;
    log_line("upscaler: worker running");
    return true;
}

void UpscaleWorker::stop()
{
    if (started_)
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        pthread_join(thread_, nullptr);
        started_ = false;
    }
    delete pool_;
    pool_ = nullptr;
    vd_upscaler_destroy(up_);
    up_ = nullptr;
    for (auto &p : in_)
    {
        free(p);
        p = nullptr;
    }
    for (auto &p : out_)
    {
        free(p);
        p = nullptr;
    }
}

void UpscaleWorker::submit(const uint8_t *nv12, int w, int h, int mode, int sharpness)
{
    if (!started_ || !supported(w, h) || mode == VD_UPSCALE_OFF)
        return;
    const size_t bytes = (size_t)w * h * 3 / 2;
    if (bytes > kMaxIn)
        return;
    {
        std::lock_guard<std::mutex> lock(m_);
        memcpy(in_[pending_], nv12, bytes);
        in_w_ = w;
        in_h_ = h;
        in_mode_ = mode;
        in_sharp_ = sharpness;
        in_gen_ = gen_;
        pending_ready_ = true;
    }
    cv_.notify_one();
}

const uint32_t *UpscaleWorker::latest(uint64_t *seq)
{
    if (!started_)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_);
    if (ready_new_)
    {
        const int t = read_;
        read_ = ready_;
        ready_ = t;
        read_gen_ = ready_gen_;
        read_seq_ = ready_seq_;
        ready_new_ = false;
    }
    if (seq)
        *seq = read_seq_;
    return read_gen_ == gen_ ? out_[read_] : nullptr;
}

void UpscaleWorker::reset()
{
    std::lock_guard<std::mutex> lock(m_);
    ++gen_;
    pending_ready_ = false;
    fps_ = 0;
}

void UpscaleWorker::run()
{
    if (core_ >= 0 && cpu_pin_current(core_) != 0)
        log_line("upscaler: pinning the worker to core %d failed", core_);
    double window_start = now_seconds();
    int window_frames = 0;
    bool logged_failure = false;
    for (;;)
    {
        int w, h, mode, sharp;
        long gen;
        const uint8_t *nv12;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [&] { return quit_ || pending_ready_; });
            if (quit_)
                return;
            pending_ready_ = false;
            const int work = pending_;
            pending_ ^= 1; // the UI fills the other buffer meanwhile
            nv12 = in_[work];
            w = in_w_;
            h = in_h_;
            mode = in_mode_;
            sharp = in_sharp_;
            gen = in_gen_;
        }

        if (!up_ || vd_upscaler_src_w(up_) != w || vd_upscaler_src_h(up_) != h)
        {
            vd_upscaler_destroy(up_);
            up_ = vd_upscaler_create(w, h);
            if (!up_)
            {
                if (!logged_failure)
                    log_line("upscaler: out of memory for %dx%d", w, h);
                logged_failure = true;
                continue;
            }
        }

        const bool trace = frames_done_ < 2; // step-by-step log of the first frames
        if (trace)
            log_line("upscaler: frame %d %dx%d %s", frames_done_ + 1, w, h,
                     vd_upscale_label(mode));
        // FSR 2's sharpening: slider 0..1 (here in quarters), denoise on.
        vd_upscaler_set_sharpness(up_, (float)sharp / 4.0f, 1);
        const double t0 = now_seconds();
        ConvertJob cj{up_, nv12};
        pool_->run(convert_rows, &cj, h);
        if (trace)
            log_line("upscaler:   colour done");
        if (mode == VD_UPSCALE_FSR)
        {
            pool_->run(analyze_rows, up_, h);
            if (trace)
                log_line("upscaler:   edges done");
        }
        RenderJob rj;
        rj.u = up_;
        rj.mode = mode;
        rj.dst = out_[write_]; // only this thread touches write_
        rj.row_offset = (h * 2 - kOutH) / 2;
        rj.trace = trace;
        pool_->run(render_rows, &rj, kOutH);
        const double t1 = now_seconds();
        if (trace)
            log_line("upscaler:   scale + sharpen done (%.0f ms)", (t1 - t0) * 1000.0);
        if (rj.failed.load())
        {
            if (!logged_failure)
                log_line("upscaler: row scratch allocation failed");
            logged_failure = true;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_);
            const int t = ready_;
            ready_ = write_;
            write_ = t;
            ready_gen_ = gen;
            ready_seq_ = ++next_seq_;
            ready_new_ = true;
        }
        ms_ = (float)((t1 - t0) * 1000.0);
        if (++frames_done_ % 600 == 0)
            log_line("upscaler: %d frames, %.0f ms each, %.1f fps", frames_done_, ms_.load(),
                     fps_.load());
        ++window_frames;
        if (t1 - window_start >= 3.0)
        {
            window_start = t1; // after a pause: start a fresh measurement
            window_frames = 1;
        }
        else if (t1 - window_start >= 1.0)
        {
            fps_ = (float)(window_frames / (t1 - window_start));
            window_frames = 0;
            window_start = t1;
        }
    }
}

} // namespace vd4
