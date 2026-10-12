// VITA4 - core pinning on the PS4 (libkernel). See cpu.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu.h"

#include <pthread.h>

#include <orbis/libkernel.h>

namespace vd4
{

namespace
{
// The OpenOrbis header declares these without parameters; the real calls are
// (thread, mask) and (thread, &mask), mask being a 64-bit core bitmap.
typedef int (*SetAffinity)(pthread_t, uint64_t);
typedef int (*GetAffinity)(pthread_t, uint64_t *);
} // namespace

int cpu_pin_current(int core)
{
    core %= kGameCores;
    if (core < 0)
        core += kGameCores;
    return reinterpret_cast<SetAffinity>(&scePthreadSetaffinity)(pthread_self(),
                                                                 (uint64_t)1 << core);
}

int cpu_allow_all()
{
    return reinterpret_cast<SetAffinity>(&scePthreadSetaffinity)(
        pthread_self(), ((uint64_t)1 << kGameCores) - 1);
}

uint64_t cpu_mask_current()
{
    uint64_t mask = 0;
    if (reinterpret_cast<GetAffinity>(&scePthreadGetaffinity)(pthread_self(), &mask) != 0)
        return 0;
    return mask;
}

int cpu_current()
{
    return sceKernelGetCurrentCpu();
}

} // namespace vd4
