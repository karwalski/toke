/* SPDX-License-Identifier: MIT */
/* tk_c99_string.h -- <string.h> with C99 function semantics on every libc.
 *
 * Story 127.439.  This tree is C99 (-std=c99) and defines _GNU_SOURCE for
 * POSIX and GNU extensions.  Recent glibc (the Ubuntu 26.04 image that
 * ubuntu-latest CI runners use) reads _GNU_SOURCE as "every feature set,
 * C23 included" and then defines memchr, strchr, strpbrk, strrchr and strstr
 * as C23 qualifier-preserving MACROS built on _Generic.  clang under
 * -std=c99 -Wpedantic reports every expansion as "'_Generic' is a C11
 * extension", and -Werror turns each call site into an error, so no clang
 * build on that image (lint, sanitizers, coverage, fuzz) got past its first
 * file.
 *
 * C99 7.1.4p1 is the sanctioned way out: "Any macro definition of a
 * function can be suppressed locally ... by #undef'ing the name; the
 * function is then the real function".  Undefining these five names gives
 * back exactly the C99 functions the code was written against (char *
 * results, as on macOS and older glibc).  It suppresses no diagnostic: no
 * warning flag changes, and the macros are removed only on glibc.
 *
 * Include this header instead of, or after, <string.h> in any file that
 * calls one of the five.  The include guard keeps a later <string.h> from
 * redefining them (glibc's own guard has already been taken).
 */
#ifndef TK_C99_STRING_H
#define TK_C99_STRING_H

#include <string.h>

#if defined(__GLIBC__) && !defined(__cplusplus)
#  undef memchr
#  undef strchr
#  undef strpbrk
#  undef strrchr
#  undef strstr
#endif

#endif /* TK_C99_STRING_H */
