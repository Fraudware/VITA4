// VITA4 - host stand-ins for cpu.h (tests only).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu.h"

namespace vd4
{
int cpu_pin_current(int)
{
    return 0;
}
int cpu_allow_all()
{
    return 0;
}
uint64_t cpu_mask_current()
{
    return 0;
}
int cpu_current()
{
    return -1;
}
} // namespace vd4
