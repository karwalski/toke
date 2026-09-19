/*
 * zip.h — C interface for the std.zip standard library module.
 *
 * Read-only access to zip containers, backed by the vendored miniz
 * (stdlib/vendor/miniz/, MIT, pinned at upstream tag 3.0.2).  Store (method 0)
 * and Deflate (method 8) are the only compression methods accepted; every
 * other method, and every encrypted entry, is rejected at open.
 *
 * WHY READ-ONLY: zip containers underlie XLSX, ODS, DOCX, EPUB and most
 * government bulk-data downloads.  Reading them is what unblocks those
 * consumers; writing has no named consumer yet, so the writer half of miniz is
 * compiled out entirely (MINIZ_NO_DEFLATE_APIS in zip.c).
 *
 * THE SECURITY MODEL IS THE INTERFACE, NOT AN ADD-ON.  An archive is
 * attacker-controlled input, so `zip_open_*` validates the WHOLE central
 * directory before it returns a handle.  A rejected archive yields NULL and a
 * reason; there is no partially-usable archive and no "skip the bad entry"
 * mode, because a consumer that iterates entries would then have to remember
 * to re-check each one.  The checks, in the order they are applied:
 *
 *   1. archive byte length      <= TK_ZIP_MAX_ARCHIVE_BYTES
 *   2. entry count              <= TK_ZIP_MAX_ENTRIES
 *   3. per entry: name is not absolute, contains no ".." component, no
 *      backslash, no embedded NUL, is non-empty and shorter than
 *      TK_ZIP_MAX_NAME (which is also miniz's own truncation point, so a name
 *      that MIGHT have been truncated is refused rather than trusted)
 *   4. per entry: method is Store or Deflate, and the entry is not encrypted
 *   5. RUNNING TOTAL of declared uncompressed sizes <= TK_ZIP_MAX_UNCOMPRESSED
 *   6. per entry: declared compression ratio <= TK_ZIP_MAX_RATIO
 *
 * Checks 5 and 6 are what make a zip bomb fail rather than exhaust memory, and
 * they are deliberately in that order: an entry that is over the size cap is
 * reported as over the size cap, not as over the ratio cap, so the two
 * rejections are distinguishable by a caller and by a test.
 *
 * The size bound is CUMULATIVE rather than per-entry on purpose.  A per-entry
 * bound is trivially defeated by splitting one bomb across many entries, and a
 * cumulative bound subsumes the per-entry case anyway (the total is never less
 * than the largest entry).  One number, no gap between the two rules.
 *
 * Sizes come from the central directory, which the archive author controls.
 * That is fine for rejection — a liar can only make their own archive be
 * refused — but it is NOT trusted for allocation: zip_read_entry() refuses to
 * write more than the declared size and re-checks the cap before allocating.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  The caller owns every returned pointer.
 *
 * Story: 135.1
 */

#ifndef TK_STDLIB_ZIP_H
#define TK_STDLIB_ZIP_H

#include <stdint.h>

/* ── Limits ──────────────────────────────────────────────────────────────
 *
 * These are compile-time constants on purpose.  Making them settable from
 * toke would put the bound under the control of whichever code path is
 * handling the archive, which is the code path most likely to be wrong.
 */

/* Largest archive this module will parse at all (bytes). */
#define TK_ZIP_MAX_ARCHIVE_BYTES   (128ULL * 1024ULL * 1024ULL)

/* Largest number of entries in one archive.  A DOCX has tens; an EPUB or a
 * bulk-data download has hundreds to thousands.  65536 is far above any
 * legitimate document and still cheap to reject. */
#define TK_ZIP_MAX_ENTRIES         65536u

/* Cumulative uncompressed size ceiling across every entry (bytes).  Note that
 * a toke `@(byte)` stores one byte per i64 slot, so a 64 MiB entry becomes a
 * 512 MiB toke array; this ceiling is the reason that stays bounded. */
#define TK_ZIP_MAX_UNCOMPRESSED    (64ULL * 1024ULL * 1024ULL)

/* Maximum declared uncompressed:compressed ratio for a single entry. Deflate
 * on ordinary document data lands under 10; the classic 42.zip layer is over
 * 1000.  200 leaves room for highly repetitive but honest data (XML, CSV). */
#define TK_ZIP_MAX_RATIO           200ULL

/* Entries at or below this size skip the ratio check: a 40-byte file that
 * deflates to 12 bytes is not a bomb, and the bound in check 5 already covers
 * it. */
#define TK_ZIP_RATIO_FLOOR         4096ULL

/* Maximum entry-name length in bytes.  This is also miniz's own
 * MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE - 1: at that length the name it reports
 * MAY have been truncated, and a truncated name must never be validated and
 * then used. */
#define TK_ZIP_MAX_NAME            511u

/* ── Types ───────────────────────────────────────────────────────────── */

/* One central-directory entry, already validated. */
typedef struct {
    char     *name;    /* heap-allocated, NUL-terminated, validated */
    uint64_t  size;    /* declared uncompressed size, bytes */
    uint64_t  csize;   /* declared compressed size, bytes */
    int       is_dir;  /* 1 if the entry is a directory record */
} TkZipEntry;

/* Opaque archive handle.  Definition lives in zip.c. */
typedef struct TkZipArchive TkZipArchive;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * zip_open_mem — validate and open an archive held in memory.
 *
 * Copies `data` (the caller keeps ownership of theirs).  Returns NULL if any
 * check above fails; the reason is always available from zip_lasterr().
 */
TkZipArchive *zip_open_mem(const uint8_t *data, uint64_t len);

/*
 * zip_open_file — read `path` and hand it to zip_open_mem.
 *
 * Present because toke has no binary file read: std.file returns a C string
 * and truncates at the first NUL, which every zip contains within its first
 * few bytes.  See the deviation note in docs/stdlib/zip.md.
 */
TkZipArchive *zip_open_file(const char *path);

/* Release an archive and every name it owns.  NULL is a no-op. */
void zip_close(TkZipArchive *a);

/* Number of validated entries.  0 for NULL. */
uint32_t zip_entry_count(const TkZipArchive *a);

/* Entry at `i`, or NULL if out of range.  Owned by the archive. */
const TkZipEntry *zip_entry_at(const TkZipArchive *a, uint32_t i);

/*
 * zip_read_entry — decompress one entry by name.
 *
 * Returns a freshly malloc'd buffer of *out_len bytes (caller frees), or NULL
 * on any failure — unknown name, a directory record, a size that no longer
 * fits the cap, or a decompression failure.  The reason is in zip_lasterr().
 *
 * A zero-length entry returns a 1-byte allocation with *out_len == 0, never
 * NULL, so "empty file" and "failed" are never the same answer.
 */
uint8_t *zip_read_entry(TkZipArchive *a, const char *name, uint64_t *out_len);

/*
 * zip_lasterr — why the most recent zip_* call in this process failed.
 *
 * The compiled error-union ABI carries no payload: `T!E` lowers to a null
 * check (docs/runtime-abi.md §7 is still unwritten), so the `$err` arm binds
 * nothing a consumer can read.  Without this, "the archive was rejected" and
 * "which of six rules rejected it" would be indistinguishable — and telling
 * them apart is the whole point of having six separate rules.  Returns "" when
 * nothing has failed.
 */
const char *zip_lasterr(void);

#endif /* TK_STDLIB_ZIP_H */
