#ifndef TK_VERSION_H
#define TK_VERSION_H
/*
 * version.h — the compiler's version, stated once for every C source.
 *
 * 135.3j (7): the version was written out by hand in four places and three of
 * them had not moved since 0.3.0: the WASM build's tkc_version() ("tkc
 * 0.3.0-wasm"), the companion frontmatter's compiler_version and the DWARF
 * producer string.  VERSION (repo root) is the ground truth;
 * scripts/verify_project_facts.py (check_version_sources) fails when this
 * value disagrees with it, or when a semver literal reappears in a source
 * that should read it from here.
 */
#define TKC_VERSION "3.1.0"

#endif /* TK_VERSION_H */
