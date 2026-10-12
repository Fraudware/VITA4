// VITA4 - logging to a file, stdout (klog) and an on-screen ring.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace vd4
{

#define VD4_LOG_DIR "/data/vita4"
#define VD4_LOG_PATH VD4_LOG_DIR "/vita4.log"

#define VD4_PREV_LOG_PATH VD4_LOG_DIR "/vita4-previous.log"

// Opens a fresh log; the last run's log is kept as VD4_PREV_LOG_PATH.
void log_open(const char *path);
// Last `max` lines of a text file into `out` (each `width` bytes). Returns count.
int log_read_tail(const char *path, char *out, int max, int width);
void log_close();
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copy the newest `max` lines (oldest first) into `out` (each `width` bytes).
int log_snapshot(char *out, int max, int width);

double now_seconds(); // monotonic

} // namespace vd4
