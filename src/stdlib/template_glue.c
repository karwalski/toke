/*
 * template_glue.c — i64-ABI wrappers for std.template module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.template.
 */

#include <stdint.h>

int64_t tk_template_render_w(int64_t tpl, int64_t data) { (void)tpl; (void)data; return 0; }
int64_t tk_template_load_w(int64_t path) { (void)path; return 0; }
int64_t tk_tpl_render_w(int64_t tpl, int64_t data) { (void)tpl; (void)data; return 0; }
int64_t tk_tpl_load_w(int64_t path) { (void)path; return 0; }
