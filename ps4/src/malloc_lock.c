// VITA4 - makes the C heap safe to use from many threads (PS4 build only).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OpenOrbis toolchain links musl's malloc, which only takes its lock once
// musl's own pthread_create has counted a second thread. VITA4's threads come
// from the PS4 system library (libkernel's pthread_create), so musl believes
// the process is single-threaded and every malloc/free runs unlocked. Two
// threads allocating at once then corrupt the heap: that is the crash hit in
// 1.06-1.08 as soon as the upscaler's threads ran next to the USB thread.
//
// The linker wraps every allocator entry point (Makefile: --wrap=...), and
// each wrapper holds one recursive spin lock around the real function.
// Recursive because musl's calloc/memalign call malloc/free internally.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__real_aligned_alloc(size_t al, size_t n);
int __real_posix_memalign(void **out, size_t al, size_t n);
void *__real_memalign(size_t al, size_t n);
void *__real_valloc(size_t n);
size_t __real_malloc_usable_size(void *p);

static _Atomic(uintptr_t) g_owner; // pthread_self() of the holder, 0 = free
static int g_depth;                // only touched by the holder

static void heap_lock(void)
{
    const uintptr_t me = (uintptr_t)pthread_self();
    if (atomic_load_explicit(&g_owner, memory_order_relaxed) == me)
    {
        ++g_depth;
        return;
    }
    unsigned spins = 0;
    for (;;)
    {
        uintptr_t expected = 0;
        if (atomic_load_explicit(&g_owner, memory_order_relaxed) == 0 &&
            atomic_compare_exchange_weak_explicit(&g_owner, &expected, me,
                                                  memory_order_acquire, memory_order_relaxed))
            break;
        if (++spins >= 64)
        {
            spins = 0;
            sched_yield();
        }
        else
            __builtin_ia32_pause();
    }
    g_depth = 1;
}

static void heap_unlock(void)
{
    if (--g_depth == 0)
        atomic_store_explicit(&g_owner, 0, memory_order_release);
}

void *__wrap_malloc(size_t n)
{
    heap_lock();
    void *p = __real_malloc(n);
    heap_unlock();
    return p;
}

void __wrap_free(void *p)
{
    if (!p)
        return;
    heap_lock();
    __real_free(p);
    heap_unlock();
}

void *__wrap_calloc(size_t a, size_t b)
{
    heap_lock();
    void *p = __real_calloc(a, b);
    heap_unlock();
    return p;
}

void *__wrap_realloc(void *q, size_t n)
{
    heap_lock();
    void *p = __real_realloc(q, n);
    heap_unlock();
    return p;
}

void *__wrap_aligned_alloc(size_t al, size_t n)
{
    heap_lock();
    void *p = __real_aligned_alloc(al, n);
    heap_unlock();
    return p;
}

int __wrap_posix_memalign(void **out, size_t al, size_t n)
{
    heap_lock();
    const int rc = __real_posix_memalign(out, al, n);
    heap_unlock();
    return rc;
}

void *__wrap_memalign(size_t al, size_t n)
{
    heap_lock();
    void *p = __real_memalign(al, n);
    heap_unlock();
    return p;
}

void *__wrap_valloc(size_t n)
{
    heap_lock();
    void *p = __real_valloc(n);
    heap_unlock();
    return p;
}

size_t __wrap_malloc_usable_size(void *p)
{
    heap_lock();
    const size_t n = __real_malloc_usable_size(p);
    heap_unlock();
    return n;
}
