#ifndef TK_GLUE_GEN_H
#define TK_GLUE_GEN_H

/*
 * glue_gen.h — Auto-generate i64-ABI _w wrappers from .tki declarations.
 *
 * Story: 7.5.5 Phase 2
 *
 * Given a stdlib .tki interface file, generates C source code for wrapper
 * functions that bridge the compiler's i64 ABI to the proper C types.
 * Only handles functions whose params and return are all "simple" types
 * (str, i64, u64, bool, void, byte).  Complex types (f64, arrays, structs,
 * error unions) are skipped — those stay in tk_web_glue.c.
 *
 * Usage:
 *   char *src = glue_gen_from_tki("/path/to/str.tki");
 *   if (src) { ... write src to a temp .c file ... free(src); }
 *
 *   char *all = glue_gen_all_stdlib("/path/to/stdlib");
 *   if (all) { ... write to temp .c file ... free(all); }
 */

/*
 * glue_gen_from_tki — Generate C wrapper source for a single .tki file.
 *
 * Returns a malloc'd string containing the generated C code, or NULL on
 * error.  Caller must free().  The generated code includes the necessary
 * #include for the module header.
 */
char *glue_gen_from_tki(const char *tki_path, const char *module_name);

/*
 * glue_gen_all_stdlib — Generate wrappers for all stdlib modules that have
 * .tki files, writing to a single combined C source string.
 *
 * stdlib_tki_dir: path to the directory containing .tki files (e.g. <repo>/stdlib/).
 * Returns a malloc'd string, or NULL.  Caller must free().
 */
char *glue_gen_all_stdlib(const char *stdlib_tki_dir);

/*
 * glue_gen_write_temp — Generate all stdlib wrappers and write to a temp file.
 *
 * stdlib_tki_dir: path to the directory containing .tki files.
 * out_path:       buffer of at least 512 bytes to receive the temp file path.
 * Returns 0 on success, -1 on failure.
 */
int glue_gen_write_temp(const char *stdlib_tki_dir, char *out_path, int out_path_sz);

#endif /* TK_GLUE_GEN_H */
