#ifndef TK_STDLIB_SYS_H
#define TK_STDLIB_SYS_H

/*
 * sys.h — C interface for the std.sys standard library module.
 *
 * Provides platform-aware config and data directory resolution.
 *
 *   macOS:  ~/Library/Application Support/<appname>
 *   Linux:  $XDG_CONFIG_HOME/<appname>  (config)
 *           $XDG_DATA_HOME/<appname>    (data)
 *
 * Story: 42.1.2
 */

/*
 * sys_configdir — return the platform config directory for appname.
 *
 * Returns a heap-allocated path; caller must free.
 * If appname is NULL or empty, returns the base directory without suffix.
 */
const char *sys_configdir(const char *appname);

/*
 * sys_datadir — return the platform data directory for appname.
 *
 * Returns a heap-allocated path; caller must free.
 * If appname is NULL or empty, returns the base directory without suffix.
 */
const char *sys_datadir(const char *appname);

#endif /* TK_STDLIB_SYS_H */
