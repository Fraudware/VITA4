// VITA4 - logging. See log.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mutex>
#include <string>
#include <vector>

namespace vd4
{

namespace
{

constexpr int kRing = 64;
constexpr int kWidth = 160;

std::mutex g_mutex;
FILE *g_file = nullptr;
char g_ring[kRing][kWidth];
int g_next = 0, g_count = 0;
double g_start = -1;

} // namespace

double now_seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void log_open(const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_start < 0)
        g_start = now_seconds();
    if (!g_file && path)
    {
        remove(VD4_PREV_LOG_PATH);
        rename(path, VD4_PREV_LOG_PATH);
        g_file = fopen(path, "w");
    }
}

int log_read_tail(const char *path, char *out, int max, int width)
{
    FILE *f = fopen(path, "r");
    if (!f || max <= 0)
    {
        if (f)
            fclose(f);
        return 0;
    }
    int count = 0, next = 0;
    char line[256];
    std::vector<std::string> ring((size_t)max);
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        ring[(size_t)next] = line;
        next = (next + 1) % max;
        if (count < max)
            ++count;
    }
    fclose(f);
    for (int i = 0; i < count; ++i)
        snprintf(out + (size_t)i * width, (size_t)width, "%s",
                 ring[(size_t)((next - count + i + max) % max)].c_str());
    return count;
}

void log_close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
    {
        fclose(g_file);
        g_file = nullptr;
    }
}

void log_line(const char *fmt, ...)
{
    char msg[kWidth - 12];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // Strip a trailing newline; lines are newline-terminated on output.
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_start < 0)
        g_start = now_seconds();
    char line[kWidth];
    snprintf(line, sizeof(line), "[%7.2f] %s", now_seconds() - g_start, msg);
    printf("[VITA4] %s\n", line);
    if (g_file)
    {
        fprintf(g_file, "%s\n", line);
        fflush(g_file);
    }
    snprintf(g_ring[g_next], kWidth, "%s", line);
    g_next = (g_next + 1) % kRing;
    if (g_count < kRing)
        ++g_count;
}

int log_snapshot(char *out, int max, int width)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const int n = g_count < max ? g_count : max;
    for (int i = 0; i < n; ++i)
    {
        const int idx = (g_next - n + i + kRing) % kRing;
        snprintf(out + (size_t)i * width, (size_t)width, "%s", g_ring[idx]);
    }
    return n;
}

} // namespace vd4
