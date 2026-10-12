// VITA4 - row-splitting render pool. See display.h.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers are plain pthreads with an explicit stack size, and a helper that
// can't be created is simply left out (fewer, wider bands) instead of
// throwing from std::thread and taking the whole app down.
#include <pthread.h>

#include <condition_variable>
#include <mutex>
#include <vector>

#include "display.h"
#include "cpu.h"
#include "log.h"

namespace vd4
{

struct RowPool::Impl
{
    std::mutex m;
    std::condition_variable start, done;
    std::vector<pthread_t> threads;
    Job job = nullptr;
    void *ctx = nullptr;
    int rows = 0;
    long generation = 0;
    int remaining = 0;
    bool quit = false;
    int bands = 1;
};

namespace
{

struct HelperArg
{
    RowPool::Impl *s;
    int index;
    int core; // -1: leave the core mask alone
};

constexpr size_t kHelperStack = 512 * 1024;

} // namespace

void *RowPool::helper_main(void *p)
{
    HelperArg arg = *static_cast<HelperArg *>(p);
    delete static_cast<HelperArg *>(p);
    if (arg.core >= 0)
    {
        const int rc = cpu_pin_current(arg.core);
        if (rc != 0)
            log_line("row pool: pinning helper %d to core %d failed (0x%x)", arg.index + 1,
                     arg.core % kGameCores, (unsigned)rc);
    }
    Impl &s = *arg.s;
    long seen = 0;
    for (;;)
    {
        std::unique_lock<std::mutex> lock(s.m);
        s.start.wait(lock, [&] { return s.quit || s.generation != seen; });
        if (s.quit)
            return nullptr;
        seen = s.generation;
        Job job = s.job;
        void *ctx = s.ctx;
        const int bands = s.bands;
        const int rows = s.rows;
        lock.unlock();
        const int b = arg.index + 1; // band 0 is the caller's
        job(ctx, rows * b / bands, rows * (b + 1) / bands, b);
        lock.lock();
        if (--s.remaining == 0)
            s.done.notify_one();
    }
}

RowPool::RowPool(int helpers, int first_core) : impl_(new Impl), helpers_(0)
{
    for (int i = 0; i < helpers; ++i)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kHelperStack);
        pthread_t t;
        HelperArg *arg = new HelperArg{impl_, i, first_core >= 0 ? first_core + i : -1};
        const int rc = pthread_create(&t, &attr, &RowPool::helper_main, arg);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            delete arg;
            log_line("row pool: helper %d of %d not created (%d)", i + 1, helpers, rc);
            break;
        }
        impl_->threads.push_back(t);
        ++helpers_;
    }
    impl_->bands = helpers_ + 1;
}

RowPool::~RowPool()
{
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->quit = true;
    }
    impl_->start.notify_all();
    for (pthread_t t : impl_->threads)
        pthread_join(t, nullptr);
    delete impl_;
}

void RowPool::run(Job job, void *ctx, int rows)
{
    Impl &s = *impl_;
    const int bands = helpers_ + 1;
    if (helpers_ == 0)
    {
        job(ctx, 0, rows, 0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s.m);
        s.job = job;
        s.ctx = ctx;
        s.rows = rows;
        s.remaining = helpers_;
        ++s.generation;
    }
    s.start.notify_all();
    job(ctx, 0, rows / bands, 0);
    std::unique_lock<std::mutex> lock(s.m);
    s.done.wait(lock, [&] { return s.remaining == 0; });
}

} // namespace vd4
