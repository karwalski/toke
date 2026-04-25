/*
 * sys.c — std.sys standard library module.
 *
 * Platform-aware config and data directory resolution.
 *
 * Story: 42.1.2
 */

#include "sys.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

/*
 * get_home — return the user's home directory.
 * Tries $HOME first, falls back to getpwuid(getuid())->pw_dir.
 * Returns NULL if both fail.
 */
static const char *get_home(void) {
    const char *h = getenv("HOME");
    if (h && h[0]) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
    return NULL;
}

/*
 * build_path — allocate "<base>/<appname>" or just "<base>" when appname
 * is NULL/empty.  Returns NULL on allocation failure.
 */
static char *build_path(const char *base, const char *appname) {
    if (!base) return NULL;
    int has_app = appname && appname[0];
    size_t len = strlen(base) + (has_app ? 1 + strlen(appname) : 0) + 1;
    char *buf = malloc(len);
    if (!buf) return NULL;
    if (has_app)
        snprintf(buf, len, "%s/%s", base, appname);
    else
        snprintf(buf, len, "%s", base);
    return buf;
}

const char *sys_configdir(const char *appname) {
#ifdef __APPLE__
    const char *home = get_home();
    if (!home) return NULL;
    char base[1024];
    snprintf(base, sizeof base, "%s/Library/Application Support", home);
    return build_path(base, appname);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return build_path(xdg, appname);
    const char *home = get_home();
    if (!home) return NULL;
    char base[1024];
    snprintf(base, sizeof base, "%s/.config", home);
    return build_path(base, appname);
#endif
}

const char *sys_datadir(const char *appname) {
#ifdef __APPLE__
    const char *home = get_home();
    if (!home) return NULL;
    char base[1024];
    snprintf(base, sizeof base, "%s/Library/Application Support", home);
    return build_path(base, appname);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) return build_path(xdg, appname);
    const char *home = get_home();
    if (!home) return NULL;
    char base[1024];
    snprintf(base, sizeof base, "%s/.local/share", home);
    return build_path(base, appname);
#endif
}
