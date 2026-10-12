// VITA4 - settings persistence. See settings_io.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv12_scale.h"
#include "pad_map.h"
#include "upscale.h"

namespace vd4
{

bool settings_load(const char *path, Settings &s)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[128];
    int version = 1;
    bool touch_seen = false;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const int v = atoi(eq + 1);
        if (strcmp(line, "touch") == 0 && v >= 0 && v < VD_TOUCH_COUNT)
        {
            s.touch_target = v;
            touch_seen = true;
        }
        else if (strcmp(line, "version") == 0)
            version = v;
        else if (strcmp(line, "passthrough") == 0)
            s.passthrough = v != 0;
        else if (strcmp(line, "picture") == 0 && v >= 0 && v < VD_SCALE_COUNT)
            s.scale_mode = v;
        else if (strcmp(line, "upscale") == 0 && v >= 0 && v < VD_UPSCALE_COUNT)
            s.upscale = v;
        else if (strcmp(line, "sharpness") == 0 && v >= 0 && v <= 4)
            s.sharpness = v;
        else if (strcmp(line, "deadzone") == 0)
            s.deadzone = v != 0;
        else if (strcmp(line, "fastpic") == 0)
            s.fast_picture = v != 0;
        else if (strcmp(line, "lowlag3") == 0) // 1.29: earlier Low lag choices are ignored
            s.low_lag = v != 0;
        else if (strcmp(line, "taps") == 0 && v >= 0 && v < VD_TAPS_COUNT)
            s.taps = v;
        else if (strcmp(line, "sound") == 0)
            s.sound = v != 0;
        else if (strcmp(line, "stats") == 0)
            s.stats = v != 0;
    }
    fclose(f);
    // Settings from 1.04 and older carry the old rear-touchpad default; the
    // VITA5 beta (2026-10-07) makes the front touchscreen the default.
    if (version < 2 && touch_seen && s.touch_target == VD_TOUCH_REAR)
        s.touch_target = VD_TOUCH_FRONT;
    return true;
}

bool settings_save(const char *path, const Settings &s)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f,
            "version=2\npassthrough=%d\ntouch=%d\npicture=%d\nupscale=%d\nsharpness=%d\nsound=%d\n"
            "stats=%d\ntaps=%d\nlowlag3=%d\nfastpic=%d\ndeadzone=%d\n",
            s.passthrough ? 1 : 0, s.touch_target, s.scale_mode, s.upscale, s.sharpness,
            s.sound ? 1 : 0, s.stats ? 1 : 0, s.taps, s.low_lag ? 1 : 0, s.fast_picture ? 1 : 0,
            s.deadzone ? 1 : 0);
    fclose(f);
    return true;
}

} // namespace vd4
