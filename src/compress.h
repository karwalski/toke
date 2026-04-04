/*
 * compress.h — Text compression/decompression API for the toke compiler.
 *
 * Provides three levels of compression:
 *   1. Placeholder-preserving prose compression (story 13.1.1)
 *   2. Streaming compression API (story 13.1.4)
 *   3. Schema-aware structured-data compression (story 13.1.5)
 *
 * Atom preservation contract
 * --------------------------
 * Any token matching the pattern  $[A-Z][A-Z0-9_]*[0-9]+  (e.g. $PERSON_1,
 * $EMAIL_3, $ADDR_12) is treated as an opaque atom: it is never split,
 * rewritten, or conflated with the type-sigil use of '$'.  Decompress()
 * restores byte-identical output for all such atoms.
 *
 * Stories: 13.1.1, 13.1.4, 13.1.5
 */

#ifndef TK_COMPRESS_H
#define TK_COMPRESS_H

#include <stddef.h>
#include <stdio.h>

/* ── Output type detection ──────────────────────────────────────────── */

typedef enum {
    COMPRESS_PROSE = 0,   /* plain natural language or unknown input  */
    COMPRESS_JSON  = 1,   /* JSON object or array                     */
    COMPRESS_CSV   = 2    /* comma-separated values with header row   */
} CompressInputType;

/*
 * detect_input_type — Heuristically classify the input as JSON, CSV, or
 *                     prose.  The detection is based on the first non-
 *                     whitespace characters and structural markers; it
 *                     never modifies the input buffer.
 *
 * input : byte buffer (need not be NUL-terminated)
 * len   : length of input in bytes
 *
 * Returns: COMPRESS_JSON, COMPRESS_CSV, or COMPRESS_PROSE.
 */
CompressInputType detect_input_type(const char *input, size_t len);

/* ── Batch compress/decompress ──────────────────────────────────────── */

/*
 * compress_text — Encode input text to a reduced-token form.
 *
 * All placeholder atoms matching $[A-Z][A-Z0-9_]*[0-9]+ are preserved
 * verbatim.  Non-atom text is compressed using whitespace normalisation,
 * common-word abbreviation, and repeated-token deduplication.
 *
 * input   : input text buffer (need not be NUL-terminated)
 * len     : length of input in bytes
 * out_buf : caller-supplied output buffer; must be at least (len + 16)
 *           bytes.  In the worst case (nothing compressible) the output
 *           is equal in length to the input.
 *
 * Returns: number of bytes written to out_buf (excluding any NUL
 *          terminator), or -1 on error.
 */
int compress_text(const char *input, size_t len, char *out_buf);

/*
 * decompress_text — Reverse compress_text, restoring byte-identical output.
 *
 * input   : compressed buffer as produced by compress_text()
 * len     : length of compressed buffer
 * out_buf : caller-supplied output buffer; must be at least (len * 4 + 64)
 *           bytes to accommodate worst-case expansion.
 *
 * Returns: number of bytes written to out_buf (excluding any NUL
 *          terminator), or -1 on error.
 */
int decompress_text(const char *input, size_t len, char *out_buf);

/* ── Schema-aware structured compression ────────────────────────────── */

/*
 * compress_json — Compress a JSON object or array by extracting the
 *                 schema once and encoding values positionally.
 *
 * The output format is:
 *   TK:JSON:<schema_hash> <key1>,<key2>,...\n
 *   <val1>\t<val2>\t...\n
 *   (one row per top-level array element, or one row for a single object)
 *
 * Falls back to compress_text() for non-object/array JSON.
 *
 * input   : JSON input buffer
 * len     : length in bytes
 * out_buf : output buffer (caller-supplied, >= len + 64 bytes)
 *
 * Returns: bytes written, or -1 on error.
 */
int compress_json(const char *input, size_t len, char *out_buf);

/*
 * compress_csv — Compress CSV by emitting the header schema once and
 *                encoding rows as tab-separated positional tuples.
 *
 * The output format is:
 *   TK:CSV:<col_count> <col1>,<col2>,...\n
 *   <val1>\t<val2>\t...\n
 *   (one line per data row)
 *
 * Falls back to compress_text() if no header row can be parsed.
 *
 * input   : CSV input buffer
 * len     : length in bytes
 * out_buf : output buffer (caller-supplied, >= len + 64 bytes)
 *
 * Returns: bytes written, or -1 on error.
 */
int compress_csv(const char *input, size_t len, char *out_buf);

/* ── Streaming compress API ─────────────────────────────────────────── */

/*
 * CompressStreamCtx — opaque streaming context.
 *
 * Callers treat this as an opaque value; the internal fields are an
 * implementation detail.  Initialise with compress_stream_init() and
 * release with compress_stream_flush().
 *
 * The streaming API is compatible with SSE and chunked HTTP delivery:
 * each call to compress_stream_feed() may emit zero or more compressed
 * chunks immediately without buffering the full input.
 */
#define COMPRESS_STREAM_BUFSZ 4096

typedef struct {
    char   pending[COMPRESS_STREAM_BUFSZ]; /* unprocessed partial token   */
    int    pending_len;                    /* bytes valid in pending[]     */
    FILE  *out;                            /* output sink (set by init)    */
    int    total_in;                       /* bytes fed so far             */
    int    total_out;                      /* bytes emitted so far         */
} CompressStreamCtx;

/*
 * compress_stream_init — Initialise a streaming compression context.
 *
 * ctx : pointer to a caller-allocated CompressStreamCtx
 * out : output FILE* to which compressed chunks are written
 */
void compress_stream_init(CompressStreamCtx *ctx, FILE *out);

/*
 * compress_stream_feed — Feed a chunk of input to the streaming compressor.
 *
 * Processes complete tokens from the accumulated buffer, emits compressed
 * output immediately.  Incomplete tokens at the end of the chunk are
 * held in ctx->pending for the next call.
 *
 * ctx   : streaming context (must have been initialised)
 * chunk : pointer to input data
 * len   : number of bytes in chunk
 *
 * Returns: number of output bytes emitted, or -1 on error.
 */
int compress_stream_feed(CompressStreamCtx *ctx, const char *chunk, int len);

/*
 * compress_stream_flush — Flush any remaining pending input and finalise.
 *
 * After this call the ctx is in an undefined state and must not be reused
 * without calling compress_stream_init() again.
 *
 * Returns: number of additional output bytes emitted, or -1 on error.
 */
int compress_stream_flush(CompressStreamCtx *ctx);

#endif /* TK_COMPRESS_H */
