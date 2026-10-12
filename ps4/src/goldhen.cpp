// VITA4 - GoldHEN helpers. See goldhen.h.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDK command interface (syscall 500, commands 0 = version, 2 = jailbreak)
// follows the GoldHEN Plugins SDK (MIT, https://github.com/GoldHEN/GoldHEN_Plugins_SDK).
#include "goldhen.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

extern "C" {
// Indirect syscall: syscall(0) with the real number in the first argument.
long vd4_orbis_syscall(long num, ...);
}

__asm__(".att_syntax prefix\n"
        ".globl vd4_orbis_syscall\n"
        "vd4_orbis_syscall:\n"
        "  movq $0, %rax\n"
        "  movq %rcx, %r10\n"
        "  syscall\n"
        "  jb 1f\n"
        "  retq\n"
        "1:\n"
        "  movq $-1, %rax\n"
        "  retq\n");

namespace vd4
{

namespace
{

constexpr long kSdkSyscall = 500;
constexpr long kCmdVersion = 0;
constexpr long kCmdJailbreak = 2;

// Credentials/dirs saved by GoldHEN so the jailbreak could be undone.
struct JailbreakBackup
{
    uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void *cr_prison;
    void *fd_cdir, *fd_jdir, *fd_rdir;
};

JailbreakBackup g_backup;

bool interesting(const char *n)
{
    static const char *keys[] = {"ugen", "usb", "uhid", "uaudio", "dsp", "audio", "video",
                                 "cam", "mtp", "da"};
    for (const char *k : keys)
        if (strncmp(n, k, strlen(k)) == 0)
            return true;
    return false;
}

} // namespace

uint32_t goldhen_version()
{
    // An unknown syscall raises SIGSYS, which would end the app on a GoldHEN
    // without the SDK interface. Ignored, the call just fails with ENOSYS.
    // FreeBSD's SIGSYS is 12; the musl headers say 31. Ignore both (the other
    // number is SIGUSR2 on the respective system, which the app never uses).
    signal(12, SIG_IGN);
    signal(SIGSYS, SIG_IGN);
    const long v = vd4_orbis_syscall(kSdkSyscall, kCmdVersion, (void *)0);
    return v < 0 ? 0 : (uint32_t)v;
}

int goldhen_jailbreak()
{
    memset(&g_backup, 0, sizeof(g_backup));
    return (int)vd4_orbis_syscall(kSdkSyscall, kCmdJailbreak, &g_backup);
}

int survey_dev(char *summary, int cap)
{
    if (summary && cap)
        summary[0] = 0;
    DIR *d = opendir("/dev");
    if (!d)
    {
        log_line("/dev: can't open (errno %d)", errno);
        if (summary)
            snprintf(summary, (size_t)cap, "/dev not readable");
        return 0;
    }
    int total = 0, ugen = 0, used = 0;
    char line[150];
    int lp = 0;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        ++total;
        if (strncmp(e->d_name, "ugen", 4) == 0)
            ++ugen;
        if (!interesting(e->d_name))
            continue;
        if (summary && used < cap - 16)
            used += snprintf(summary + used, (size_t)(cap - used), "%s%s", used ? " " : "",
                             e->d_name);
        // Log in lines of a sensible width.
        const int need = (int)strlen(e->d_name) + 1;
        if (lp + need >= (int)sizeof(line) - 1)
        {
            log_line("/dev: %s", line);
            lp = 0;
        }
        lp += snprintf(line + lp, sizeof(line) - (size_t)lp, "%s%s", lp ? " " : "", e->d_name);
    }
    closedir(d);
    if (lp)
        log_line("/dev: %s", line);
    log_line("/dev: %d entries, %d ugen node(s)", total, ugen);
    return ugen;
}

} // namespace vd4
