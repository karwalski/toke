/* SPDX-License-Identifier: MIT */
#ifndef TK_STRHDR_H
#define TK_STRHDR_H
/*
 * tk_strhdr.h — length-carrying strings: the sealed string heap
 * (story 127.170, docs/runtime-abi.md §3.1, option S1).
 *
 * A toke string stays a NUL-terminated `char *`.  Nothing that reads one with
 * strlen() is wrong.  What this adds is a way to know the length in O(1) when
 * the string came from a CONVERTED producer:
 *
 *     [ len:i64 | b0 b1 ... b(len-1) \0 | pad to 16 ]
 *                 ^ the pointer toke sees: 16-aligned + 8
 *
 * Converted producers allocate from one reserved address range (tks_alloc),
 * write the bytes and the NUL, then call tks_seal(p, n), which writes the
 * header and sets one bit in a side bitmap (one bit per 16-byte granule).
 * A reader trusts p[-8..-1] ONLY when p lies in the range, sits at a granule's
 * +8, and that granule's bit is set; otherwise it calls strlen.  So:
 *
 *   - an unconverted producer is slow and correct (no bit -> strlen);
 *   - an unconverted reader is slow and correct (the bytes are a C string);
 *   - literals, argv, static buffers, malloc'd strings and INTERIOR pointers
 *     (a pointer into the middle of a sealed string has no bit) are excluded
 *     by construction, with no list to maintain.
 *
 * THE RULE (owner decision 2026-10-01 §2.1): a sealed string is never written,
 * freed or reallocated.  free()/realloc() of a sealed string aborts loudly
 * (the address did not come from malloc).  A write is caught by the verify
 * switch: with TK_STR_VERIFY=1 in the running program's environment (read
 * once, when the heap is first used) every trusted read asserts
 * header == strlen(p) and aborts on a mismatch; -DTK_STR_VERIFY=1 at compile
 * time forces it on.
 *
 * The header MUST equal strlen(p).  A producer whose bytes can contain a 0
 * (str.addbyte(b;0), str.frombytes of a buffer holding a 0) seals with the
 * C-string length, not the count it wrote.
 *
 * Memory: TKS_RESERVE bytes of address space are reserved PROT_NONE at the
 * first tks_alloc, with a bitmap of TKS_RESERVE/128 bytes, also PROT_NONE.
 * Both are committed (mprotect'd read/write) in TKS_CHUNK steps as the bump
 * pointer reaches them, so the commit charge follows use on Linux under any
 * overcommit mode and RSS follows touched pages everywhere.  Strings are never
 * freed — the same as every other toke string today (runtime-abi §3).
 *
 * Fallback: where the reservation cannot be made (no mmap — wasm; a 32-bit
 * size_t; RLIMIT_AS too small; mprotect refused) tks_alloc returns malloc'd
 * memory with no header and no bit: every string unsealed, behaviour
 * identical to before this header existed.  Defining TK_STR_NO_HEAP forces
 * that fallback.
 *
 * Threads: allocation is an atomic bump; the one-time reservation and each
 * chunk commit happen under a spin lock; tks_seal publishes the bit with
 * release ordering and readers load it with acquire, so a reader on another
 * thread sees either the header or strlen, never half of one.
 *
 * The state and the out-of-line half live in str.c, which every compiled
 * toke program and the compiler itself link.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef TK_STR_VERIFY
#define TK_STR_VERIFY 0
#endif

#if !defined(TK_STR_NO_HEAP) && SIZE_MAX > 0xFFFFFFFFu && \
    !defined(__wasm__) && !defined(__wasi__) && !defined(_WIN32)
#define TKS_HEAP 1
#else
#define TKS_HEAP 0
#endif

#define TKS_RESERVE ((uint64_t)1 << 34)     /* 16 GiB of address space */
#define TKS_CHUNK   ((size_t)64 << 20)      /* committed 64 MiB at a time */

extern char       *tks_lo;      /* region base; NULL until (and unless) reserved */
extern uint8_t    *tks_bits;    /* one bit per 16-byte granule of the region */
extern int         tks_verify;  /* 1 = every trusted read asserts header == strlen */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, cold))
#endif
void tks_verify_fail(const char *p, int64_t hdr);

/* Room for n bytes plus the NUL.  The result has no header until tks_seal().
 * Never NULL unless malloc itself fails. */
char *tks_alloc(size_t n);

/* Publish the length.  p must come from tks_alloc(m) with n <= m, and the
 * bytes p[0..n] must be final with p[n] == '\0' and no 0 byte before it.
 * Returns p.  A pointer that is not a heap allocation is returned unchanged
 * (the malloc fallback). */
char *tks_seal(char *p, size_t n);

/* Copy n bytes into a new string and seal it.  s[0..n) must hold no 0 byte. */
char *tks_dup(const char *s, size_t n);

/* tks_dup of a C string, measuring it with tk_strlen. */
char *tks_strdup(const char *s);

/* The byte length carried by p, or -1 when p carries none. */
static inline int64_t tks_hdrlen(const char *p)
{
#if TKS_HEAP
    const char *lo = __atomic_load_n(&tks_lo, __ATOMIC_ACQUIRE);
    if (!lo) return -1;
    uint64_t o = (uint64_t)((uintptr_t)p - (uintptr_t)lo);
    if (o >= TKS_RESERVE || (o & 15) != 8) return -1;
    uint64_t g = o >> 4;
    if (!((__atomic_load_n(&tks_bits[g >> 3], __ATOMIC_ACQUIRE) >> (g & 7)) & 1))
        return -1;
    int64_t n;
    /* 127.439: read the header through lo, the region it lives in (lo + o
     * == p).  Spelled p - 8, gcc 15 inlined this into callers passing a
     * stack buffer and reported -Warray-bounds on that path, which the
     * range check above has already returned from. */
    memcpy(&n, lo + (o - 8), sizeof n);
    if ((TK_STR_VERIFY || tks_verify) && (size_t)n != strlen(p))
        tks_verify_fail(p, n);
    return n;
#else
    (void)p;
    return -1;
#endif
}

/* The length every converted reader uses: O(1) for a sealed string, strlen
 * for anything else.  p must not be NULL. */
static inline size_t tk_strlen(const char *p)
{
    int64_t n = tks_hdrlen(p);
    return n >= 0 ? (size_t)n : strlen(p);
}

#endif /* TK_STRHDR_H */
