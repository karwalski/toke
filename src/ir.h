#ifndef TK_IR_H
#define TK_IR_H

/*
 * ir.h — Interface file emitter for the toke reference compiler.
 *
 * Emits a .tki JSON file alongside the compiled binary. The .tki file
 * records all exported symbols with their kinds and signatures so that
 * dependent modules can be type-checked without the source.
 *
 * Error codes:
 *   E9001 — failed to write interface file (I/O error)
 *
 * Story: 1.2.7  Branch: feature/compiler-interface-emitter
 */

#include <stdint.h>
#include "types.h"

/* ── Error codes ─────────────────────────────────────────────────────── */

#define E9001 9001  /* failed to write interface file                    */

/* ── Public API ───────────────────────────────────────────────────────── */

/*
 * emit_interface — write a .tki JSON file for the compiled module.
 *
 * ast        : the parsed AST (for module path and declarations)
 * src        : original source buffer
 * type_env   : completed type environment (for signature types)
 * out_path   : file path to write (e.g. "std/http.tki")
 *
 * Returns 0 on success, -1 on I/O error (emits internal diagnostic E9001).
 */
int emit_interface(const Node *ast, const char *src,
                   const TypeEnv *type_env, const char *out_path);

/*
 * interface_hash — compute a stable 32-bit content hash of the .tki file
 * at path. Used by incremental compilation to detect whether the exported
 * interface has changed. Uses djb2 over the raw file bytes.
 * Returns 0 if the file cannot be read.
 */
uint32_t interface_hash(const char *path);

#endif /* TK_IR_H */
