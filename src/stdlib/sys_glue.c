/*
 * sys_glue.c — i64-ABI wrappers for std.sys module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.sys.
 */

#include "sys.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/statvfs.h>
#include <mach/mach.h>
#else
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#endif

/* sys.platform() — return "macos", "linux", or "unknown" */
int64_t tk_sys_platform_w(int64_t dummy) {
    (void)dummy;
#ifdef __APPLE__
    return (int64_t)(intptr_t)"macos";
#elif defined(__linux__)
    return (int64_t)(intptr_t)"linux";
#else
    return (int64_t)(intptr_t)"unknown";
#endif
}

/* sys.isarm64() — return 1 if running on ARM64 architecture */
int64_t tk_sys_isarm64_w(int64_t dummy) {
    (void)dummy;
#if defined(__aarch64__) || defined(__arm64__)
    return 1;
#else
    return 0;
#endif
}

/* sys.cpucount() — return number of available CPU cores */
int64_t tk_sys_cpucount_w(int64_t dummy) {
    (void)dummy;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 1;
}

/* sys.totalramgb() — return total physical RAM in gigabytes */
int64_t tk_sys_totalramgb_w(int64_t dummy) {
    (void)dummy;
#ifdef __APPLE__
    int mib[2] = { CTL_HW, HW_MEMSIZE };
    uint64_t ram = 0;
    size_t len = sizeof(ram);
    sysctl(mib, 2, &ram, &len, NULL, 0);
    return (int64_t)(ram / (1024ULL * 1024 * 1024));
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return (int64_t)(si.totalram * si.mem_unit / (1024ULL * 1024 * 1024));
    return 0;
#endif
}

/* sys.diskfreegb(path) — return free disk space in GB for the given path */
int64_t tk_sys_diskfreegb_w(int64_t path) {
    const char *p = path ? (const char *)(intptr_t)path : "/";
    struct statvfs st;
    if (statvfs(p, &st) != 0) return 0;
    uint64_t free_bytes = (uint64_t)st.f_bavail * st.f_frsize;
    return (int64_t)(free_bytes / (1024ULL * 1024 * 1024));
}

/* sys.chipname() — return CPU chip/model name */
int64_t tk_sys_chipname_w(int64_t dummy) {
    (void)dummy;
#ifdef __APPLE__
    char buf[256];
    size_t len = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, NULL, 0) == 0) {
        return (int64_t)(intptr_t)strdup(buf);
    }
    return (int64_t)(intptr_t)"Apple Silicon";
#else
    return (int64_t)(intptr_t)"unknown";
#endif
}

/* sys.configdir(appname) — platform config directory */
int64_t tk_sys_configdir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_configdir(
        appname ? (const char *)(intptr_t)appname : "");
}

/* sys.datadir(appname) — platform data directory */
int64_t tk_sys_datadir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_datadir(
        appname ? (const char *)(intptr_t)appname : "");
}
