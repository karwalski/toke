/*
 * config.h — tkc.toml configuration file support.
 *
 * Loads project-wide compiler limits from a TOML file.
 * Precedence: CLI flags > tkc.toml > compiled defaults.
 *
 * Story: 10.11.8
 */

#ifndef TKC_CONFIG_H
#define TKC_CONFIG_H

#include "tkc_limits.h"

/*
 * tkc_load_config — parse a tkc.toml file and apply values to limits.
 *
 * Returns:
 *    0  success (file found and parsed)
 *   -1  file not found (not an error; defaults remain)
 *   -2  parse error (malformed line)
 */
int tkc_load_config(const char *path, TkcLimits *limits);

#endif /* TKC_CONFIG_H */
