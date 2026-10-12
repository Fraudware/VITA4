// VITA4 - which CPU cores a thread may run on.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the PS4 a new thread can inherit its creator's core mask; if that is a
// single core, every render thread queues on it and "parallel" work runs one
// band at a time. VITA4 therefore spreads its threads over the six game cores
// explicitly. The host build (tests) has no-op versions.
#pragma once
#include <stdint.h>

namespace vd4
{

constexpr int kGameCores = 6; // PS4 games own cores 0-5

// Pin the calling thread to one core (core % kGameCores). Returns 0 or an error.
int cpu_pin_current(int core);
// Let the calling thread run on every game core. Returns 0 or an error.
int cpu_allow_all();
// The calling thread's core mask (0 if unknown) and current core (-1 if unknown).
uint64_t cpu_mask_current();
int cpu_current();

} // namespace vd4
