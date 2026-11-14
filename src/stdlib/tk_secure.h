/* SPDX-License-Identifier: MIT */
/*
 * tk_secure.h — the stdlib crypto core's random source and secret wiping
 * (story 121.20, audit 120.9 findings CRY-01 and HYG-01).
 *
 * tk_random_bytes(buf, n)
 *   Fills buf with n bytes from the operating system's CSPRNG, or ABORTS.
 *   It never returns with buf unfilled.  Before 121.20 encrypt.c's
 *   random_bytes() fell back to memset(buf, 0, n) on every non-Apple/BSD
 *   platform when /dev/urandom could not be opened or read (a chroot, a
 *   seccomp jail, fd exhaustion), so keys, nonces, private scalars, RSA
 *   primes, OAEP seeds and PSS salts were silently ALL ZERO; crypto.c's
 *   glibc<2.36 arc4random_buf shim ignored fread's result, leaving bcrypt
 *   salts and crypto.randombytes output uninitialised.  A predictable key
 *   is undetectable at the call site and there is no safe value to return,
 *   so the only fail-closed behaviour is to stop -- which is also what
 *   glibc's and OpenBSD's own arc4random do when they cannot get entropy.
 *
 *   Sources, in order:
 *     Apple / FreeBSD / OpenBSD / NetBSD   arc4random_buf (cannot fail)
 *     Linux with <sys/random.h>            getrandom(2), no fd needed
 *     everything, and the fallback         TK_URANDOM_PATH, read in full
 *
 *   TK_RNG_PORTABLE (compile-time) skips the first two, and TK_URANDOM_PATH
 *   (default "/dev/urandom") names the device: test/conform/C452 builds the
 *   crypto core with both to prove an unreadable device aborts instead of
 *   producing a key.  Neither is set by any shipped build.
 *
 * tk_secure_zero(p, n)
 *   Zeroes n bytes in a way the compiler may not elide as a dead store --
 *   explicit_bzero where libc has it, else memset_s, else a volatile loop
 *   (the same order as securemem.c).  Used on key blocks, key schedules,
 *   KDF intermediates and private-key copies before they go out of scope.
 *
 * Header-only (static), like strbuf.h, so no module's link set changes.
 */
#ifndef TK_SECURE_H
#define TK_SECURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#ifndef TK_URANDOM_PATH
#define TK_URANDOM_PATH "/dev/urandom"
#endif

#if !defined(TK_RNG_PORTABLE) && \
    (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__))
#  define TK_RNG_ARC4 1
#elif !defined(TK_RNG_PORTABLE) && defined(__linux__) && defined(__has_include)
#  if __has_include(<sys/random.h>)
#    include <sys/random.h>
#    define TK_RNG_GETRANDOM 1
#  endif
#endif

static inline void tk_secure_zero(void *ptr, size_t n)
{
    if (!ptr || n == 0) return;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(ptr, n);
#elif defined(__STDC_LIB_EXT1__)
    memset_s(ptr, n, 0, n);
#else
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    for (size_t i = 0; i < n; i++) p[i] = 0;
#endif
}

static inline void tk_random_bytes(void *buf, size_t n)
{
    if (n == 0) return;
#if defined(TK_RNG_ARC4)
    arc4random_buf(buf, n);
#else
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
#  if defined(TK_RNG_GETRANDOM)
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;                      /* ENOSYS etc.: try the device */
        }
        got += (size_t)r;
    }
#  endif
    if (got < n) {
        FILE *f = fopen(TK_URANDOM_PATH, "rb");
        if (f) {
            /* fread returns short only at EOF or on an error, and after
             * either the stream must not be read again (127.439: clang's
             * unix.Stream on Linux).  Same result: what was read counts. */
            while (got < n) {
                got += fread(p + got, 1, n - got, f);
                if (feof(f) || ferror(f)) break;
            }
            fclose(f);
        }
    }
    if (got < n) {
        fputs("toke: no system random source (getrandom and "
              TK_URANDOM_PATH " both failed); aborting rather than "
              "generate a predictable key\n", stderr);
        abort();
    }
#endif
}

#endif /* TK_SECURE_H */
