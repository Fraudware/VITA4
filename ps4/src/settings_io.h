// VITA4 - settings persistence (key=value text file). Portable.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ui.h"

namespace vd4
{

#define VD4_SETTINGS_PATH "/data/vita4/settings.txt"

bool settings_load(const char *path, Settings &s);
bool settings_save(const char *path, const Settings &s);

} // namespace vd4
