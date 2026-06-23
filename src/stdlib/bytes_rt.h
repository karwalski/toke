/*
 * bytes_rt.h — runtime marshalling for the toke `bytes` / `[byte]` type.
 *
 * Type-flow redesign, Stage 5: a toke [byte] value is represented as a toke
 * i64-array whose elements are byte values 0-255 (block layout: u64 count at
 * ptr[-1], then count i64 elements; the pointer toke sees is and-block[1]).
 * This reuses ALL existing array machinery - .len()/.get()/+/literals work
 * unchanged - and is binary-safe (an i64 element holds 0x00 fine), unlike the
 * old NUL-terminated string stub that truncated at the first zero byte.
 *
 * These helpers marshal that representation to/from a contiguous uint8_t buffer
 * for the C crypto/encoding implementations (which take ByteArray ptr,len).
 * static inline so each glue translation unit gets its own copy - no
 * cross-glue link dependency (crypto_glue.o must not require str_glue.o).
 */
#ifndef TK_BYTES_RT_H
#define TK_BYTES_RT_H

#include <stdint.h>
#include <stdlib.h>

/* Unpack a toke [byte] (i64-array of byte values) into a freshly malloc'd
 * contiguous uint8_t buffer (NUL-terminated for convenience). Returns the
 * length; *out receives the buffer (caller frees). NULL/empty -> len 0. */
static inline uint64_t tk_bytes_unpack(int64_t arr, uint8_t **out) {
    if (!arr) { *out = (uint8_t *)malloc(1); if (*out) (*out)[0] = 0; return 0; }
    const int64_t *ptr = (const int64_t *)(intptr_t)arr;
    int64_t n = ptr[-1];
    if (n < 0) n = 0;
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf) { *out = NULL; return 0; }
    for (int64_t i = 0; i < n; i++) buf[i] = (uint8_t)(ptr[i] & 0xFF);
    buf[n] = 0;
    *out = buf;
    return (uint64_t)n;
}

/* Pack a contiguous byte buffer into a toke [byte] (i64-array of byte values).
 * Returns the toke array handle (&block[1]). */
static inline int64_t tk_bytes_pack(const uint8_t *buf, uint64_t n) {
    int64_t *block = (int64_t *)malloc((size_t)(n + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)n;
    for (uint64_t i = 0; i < n; i++) block[i + 1] = (int64_t)buf[i];
    return (int64_t)(intptr_t)(block + 1);
}

#endif /* TK_BYTES_RT_H */
