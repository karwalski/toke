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

#include <stdio.h>
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

/*
 * W9012 — 138.19 D4, the warning release: a tkc.toml [capabilities] grant
 * written as a quoted string.  It keeps its old meaning (the whole class)
 * in this release and becomes a one-element scope list in TKC_CAP_SCOPE_RELEASE.
 */
#define W9012 9012  /* tkc.toml capability grant written as a quoted string */
#define TKC_CAP_SCOPE_RELEASE "3.4.0"

/*
 * tkc_cap_flag — 138.19: a compile-time capability flag.  --allow-x bakes
 * the whole class, --allow-x=a,b bakes scopes (each validated), --allow-all
 * every class.  Returns 1 when `arg` was one (limits updated), 0 when it is
 * not a capability flag, -1 after printing why a scope is malformed.
 */
int tkc_cap_flag(const char *arg, TkcLimits *limits);

/* tkc_cap_warn_quoted — emit W9012 for every quoted-string grant recorded
 * by tkc_load_config (call once the diagnostic format is set). */
void tkc_cap_warn_quoted(const TkcLimits *limits);

/* tkc_emit_cap_globals: declared in tkc_limits.h, which llvm.c includes. */

/* tkc_cap_describe — the baked grant set, one line per class, for
 * --show-limits and the --companion record (138.19: with its scopes). */
void tkc_cap_describe(FILE *out, const TkcLimits *limits, const char *indent);

#endif /* TKC_CONFIG_H */
