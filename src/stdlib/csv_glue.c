/*
 * csv_glue.c — i64-ABI wrappers for std.csv module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.csv.
 */

#include <stdint.h>

int64_t tk_csv_parse_w(int64_t data) { (void)data; return 0; }
int64_t tk_csv_serialize_w(int64_t data) { (void)data; return 0; }
