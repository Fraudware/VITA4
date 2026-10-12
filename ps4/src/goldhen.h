// VITA4 - GoldHEN helpers: sandbox escape for raw USB access, and /dev survey.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace vd4
{

// GoldHEN SDK version (0 when GoldHEN's SDK syscall is not answering).
uint32_t goldhen_version();

// Ask GoldHEN to lift this app's sandbox (root user, real filesystem root),
// so /dev/ugen* can be opened. Returns 0 on success.
int goldhen_jailbreak();

// Log every /dev entry that looks USB- or audio-related. Returns how many
// ugen nodes were seen. `summary` (optional) receives a short one-line list.
int survey_dev(char *summary, int cap);

} // namespace vd4
