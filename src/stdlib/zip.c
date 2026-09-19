/*
 * zip.c — Implementation of the std.zip standard library module (story 135.1).
 *
 * HOW MINIZ IS COMPILED, AND WHY IT IS #included RATHER THAN LISTED
 * -----------------------------------------------------------------
 * The vendored sources are tracked in-tree exactly as story 127.85 requires
 * (stdlib/vendor/README.md), but miniz needs four compile-time -D switches
 * that cmark and tomlc99 do not, and there is no single place to put them:
 * `tkc --out` builds cflags in src/llvm.c while stdlib_deps.c contributes only
 * LINKER flags, and the `link_all` fallback path ignores per-module flags
 * altogether.  A define that is present on one path and absent on another is
 * an ODR hazard that shows up as a link error nobody can read.
 *
 * So miniz is compiled as part of THIS translation unit, with the switches set
 * right here where they cannot be lost.  The include path is relative to this
 * file, the same way toml.c reaches tomlc99, so it needs no -I either.  The
 * net effect is that std.zip needs no command-line configuration at all: the
 * Makefile path, the tkc clang path and any future path compile it
 * identically.
 *
 * MINIZ_NO_ZLIB_COMPATIBLE_NAMES is not optional: without it miniz defines
 * `compress`, `uncompress`, `crc32`, `adler32` and ZLIB_VERSION at file scope,
 * and toke links -lz.
 *
 * MINIZ_NO_DEFLATE_APIS compiles the compressor out (it also implies
 * MINIZ_NO_ARCHIVE_WRITING_APIS), which is the read-only scope of 135.1 made
 * structural rather than documented.
 */

#define MINIZ_NO_STDIO                  /* archives arrive as bytes, not FILE* */
#define MINIZ_NO_TIME                   /* entry timestamps are not exposed     */
#define MINIZ_NO_DEFLATE_APIS           /* read-only: no compressor, no writer  */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES  /* toke links -lz; do not collide       */
#include "../../stdlib/vendor/miniz/miniz.c"

#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Last-error channel ──────────────────────────────────────────────────
 *
 * See the zip_lasterr() comment in zip.h for why this exists at all.  Every
 * message is a string literal with static storage, so there is nothing to own
 * and nothing to free; the pointer stays valid until the next failure.
 */
static const char *g_zip_err = "";

static void *zip_fail(const char *msg)
{
    g_zip_err = msg ? msg : "zip: unknown error";
    return NULL;
}

const char *zip_lasterr(void)
{
    return g_zip_err;
}

/* ── Archive handle ──────────────────────────────────────────────────── */

struct TkZipArchive {
    uint8_t        *data;     /* owned copy of the archive bytes        */
    size_t          len;
    mz_zip_archive  mz;       /* reader state, points into `data`       */
    int             mz_open;  /* 1 once mz_zip_reader_init_mem succeeded */
    TkZipEntry     *entries;  /* validated central directory            */
    uint32_t        count;
};

static void zip_free_entries(TkZipArchive *a)
{
    if (!a || !a->entries) return;
    for (uint32_t i = 0; i < a->count; i++) free(a->entries[i].name);
    free(a->entries);
    a->entries = NULL;
    a->count = 0;
}

void zip_close(TkZipArchive *a)
{
    if (!a) return;
    zip_free_entries(a);
    if (a->mz_open) mz_zip_reader_end(&a->mz);
    free(a->data);
    free(a);
}

/* ── Entry-name validation ───────────────────────────────────────────────
 *
 * Returns NULL if the name is acceptable, otherwise the rejection reason.
 *
 * The rule is "the name must be a relative path that cannot escape the
 * directory a caller would extract into", enforced on the name as it appears
 * in the archive.  It is applied at OPEN, to every entry, so that a consumer
 * iterating entries cannot forget to apply it — an archive containing one bad
 * name is refused entirely rather than handed over minus that entry.
 *
 * Backslash is rejected outright rather than treated as a separator.  The zip
 * specification (APPNOTE 4.4.17.1) says names use forward slashes, so a
 * backslash is either a Windows path that some extractor will interpret as a
 * separator — making `..\..\x` a traversal that a '/'-only check misses — or
 * an unusual literal filename.  Neither is worth accepting: no XLSX, ODS,
 * DOCX or EPUB contains one.
 */
static const char *zip_name_reject(const char *name, size_t len)
{
    if (len == 0)                  return "zip: entry name is empty";
    if (len > TK_ZIP_MAX_NAME)
        return "zip: entry name exceeds the name-length cap "
               "(it may have been truncated, so it is not trusted)";

    if (name[0] == '/')            return "zip: absolute path in entry name";
    if (memchr(name, '\\', len))   return "zip: backslash in entry name";
    /* "C:" or "c:/..." — an absolute path on Windows. */
    if (len >= 2 && name[1] == ':' &&
        ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')))
        return "zip: drive-letter absolute path in entry name";

    /* Any ".." path component, at any depth: "../x", "a/../../x", "a/..". */
    {
        size_t start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || name[i] == '/') {
                size_t clen = i - start;
                if (clen == 2 && name[start] == '.' && name[start + 1] == '.')
                    return "zip: path traversal in entry name";
                start = i + 1;
            }
        }
    }
    return NULL;
}

/* ── Central-directory validation ────────────────────────────────────── */

/*
 * zip_validate — walk every central-directory record, apply checks 2-6 from
 * zip.h, and build the entry table.  Returns 0 on success; on failure sets
 * the last-error and leaves the entry table released.
 */
static int zip_validate(TkZipArchive *a)
{
    mz_uint n = mz_zip_reader_get_num_files(&a->mz);

    if (n > TK_ZIP_MAX_ENTRIES) {
        zip_fail("zip: entry count cap exceeded");
        return -1;
    }

    a->entries = (TkZipEntry *)calloc(n ? n : 1, sizeof(TkZipEntry));
    if (!a->entries) { zip_fail("zip: out of memory"); return -1; }
    a->count = 0;

    uint64_t total = 0;

    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&a->mz, i, &st)) {
            zip_fail("zip: unreadable central-directory entry");
            goto fail;
        }

        size_t nlen = strlen(st.m_filename);
        const char *why = zip_name_reject(st.m_filename, nlen);
        if (why) { zip_fail(why); goto fail; }

        if (st.m_is_encrypted) {
            zip_fail("zip: encrypted entry (unsupported)");
            goto fail;
        }
        /* 0 = Store, 8 = Deflate.  Everything else (bzip2, LZMA, zstd, xz,
         * and the "unsupported" cases miniz flags) is refused by name rather
         * than failing later inside the decompressor. */
        if (st.m_method != 0 && st.m_method != 8) {
            zip_fail("zip: unsupported compression method "
                     "(only Store and Deflate are read)");
            goto fail;
        }
        if (!st.m_is_supported) {
            zip_fail("zip: entry uses a feature this reader does not support");
            goto fail;
        }

        /* Check 5 — cumulative uncompressed size.  Accumulating BEFORE the
         * ratio test is what makes an over-size entry report as over-size. */
        if (st.m_uncomp_size > TK_ZIP_MAX_UNCOMPRESSED ||
            total > TK_ZIP_MAX_UNCOMPRESSED - st.m_uncomp_size) {
            zip_fail("zip: uncompressed size cap exceeded");
            goto fail;
        }
        total += st.m_uncomp_size;

        /* Check 6 — compression ratio.  A zero compressed size with real
         * declared content is an infinite ratio, so it is refused here too. */
        if (st.m_uncomp_size > TK_ZIP_RATIO_FLOOR) {
            if (st.m_comp_size == 0 ||
                st.m_uncomp_size / st.m_comp_size > TK_ZIP_MAX_RATIO) {
                zip_fail("zip: compression ratio cap exceeded");
                goto fail;
            }
        }

        {
            TkZipEntry *e = &a->entries[a->count];
            e->name = (char *)malloc(nlen + 1);
            if (!e->name) { zip_fail("zip: out of memory"); goto fail; }
            memcpy(e->name, st.m_filename, nlen + 1);
            e->size   = st.m_uncomp_size;
            e->csize  = st.m_comp_size;
            e->is_dir = st.m_is_directory ? 1 : 0;
            a->count++;
        }
    }
    return 0;

fail:
    zip_free_entries(a);
    return -1;
}

/* ── Open ────────────────────────────────────────────────────────────── */

TkZipArchive *zip_open_mem(const uint8_t *data, uint64_t len)
{
    g_zip_err = "";

    if (!data || len == 0)
        return (TkZipArchive *)zip_fail("zip: empty archive");
    if (len > TK_ZIP_MAX_ARCHIVE_BYTES)
        return (TkZipArchive *)zip_fail("zip: archive size cap exceeded");

    TkZipArchive *a = (TkZipArchive *)calloc(1, sizeof *a);
    if (!a) return (TkZipArchive *)zip_fail("zip: out of memory");

    a->data = (uint8_t *)malloc((size_t)len);
    if (!a->data) { free(a); return (TkZipArchive *)zip_fail("zip: out of memory"); }
    memcpy(a->data, data, (size_t)len);
    a->len = (size_t)len;

    mz_zip_zero_struct(&a->mz);
    if (!mz_zip_reader_init_mem(&a->mz, a->data, a->len, 0)) {
        zip_close(a);
        return (TkZipArchive *)zip_fail("zip: not a zip archive, or its "
                                        "central directory is corrupt");
    }
    a->mz_open = 1;

    if (zip_validate(a) != 0) {
        /* g_zip_err already names the rule that rejected it. */
        const char *keep = g_zip_err;
        zip_close(a);
        g_zip_err = keep;
        return NULL;
    }
    return a;
}

TkZipArchive *zip_open_file(const char *path)
{
    g_zip_err = "";
    if (!path || !*path)
        return (TkZipArchive *)zip_fail("zip: empty path");

    FILE *f = fopen(path, "rb");
    if (!f) return (TkZipArchive *)zip_fail("zip: cannot open archive file");

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return (TkZipArchive *)zip_fail("zip: cannot seek archive file");
    }
    long n = ftell(f);
    if (n < 0) { fclose(f); return (TkZipArchive *)zip_fail("zip: cannot size archive file"); }
    if ((uint64_t)n > TK_ZIP_MAX_ARCHIVE_BYTES) {
        fclose(f);
        return (TkZipArchive *)zip_fail("zip: archive size cap exceeded");
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return (TkZipArchive *)zip_fail("zip: out of memory"); }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return (TkZipArchive *)zip_fail("zip: short read on archive file");
    }

    TkZipArchive *a = zip_open_mem(buf, (uint64_t)n);
    free(buf);
    return a;
}

/* ── Entry access ────────────────────────────────────────────────────── */

uint32_t zip_entry_count(const TkZipArchive *a)
{
    return a ? a->count : 0u;
}

const TkZipEntry *zip_entry_at(const TkZipArchive *a, uint32_t i)
{
    if (!a || i >= a->count) return NULL;
    return &a->entries[i];
}

/* ── Read ────────────────────────────────────────────────────────────── */

uint8_t *zip_read_entry(TkZipArchive *a, const char *name, uint64_t *out_len)
{
    if (out_len) *out_len = 0;
    g_zip_err = "";

    if (!a || !name || !out_len)
        return (uint8_t *)zip_fail("zip: invalid argument");

    /* Resolve against OUR validated table, not miniz's, so a name that failed
     * validation can never be reached even if miniz would locate it. */
    const TkZipEntry *e = NULL;
    for (uint32_t i = 0; i < a->count; i++) {
        if (strcmp(a->entries[i].name, name) == 0) { e = &a->entries[i]; break; }
    }
    if (!e) return (uint8_t *)zip_fail("zip: no such entry");
    if (e->is_dir) return (uint8_t *)zip_fail("zip: entry is a directory");

    /* The declared size passed the cap at open; re-check here so that this
     * function is safe on its own terms and not only as a consequence of the
     * order it happens to be called in. */
    if (e->size > TK_ZIP_MAX_UNCOMPRESSED)
        return (uint8_t *)zip_fail("zip: uncompressed size cap exceeded");

    int idx = mz_zip_reader_locate_file(&a->mz, name, NULL, 0);
    if (idx < 0) return (uint8_t *)zip_fail("zip: no such entry");

    if (e->size == 0) {
        uint8_t *empty = (uint8_t *)malloc(1);
        if (!empty) return (uint8_t *)zip_fail("zip: out of memory");
        empty[0] = 0;
        *out_len = 0;
        return empty;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)e->size);
    if (!buf) return (uint8_t *)zip_fail("zip: out of memory");

    /* The buffer is exactly the DECLARED size, so an entry whose real
     * contents are larger than its central-directory record claims fails here
     * instead of overflowing.  miniz also verifies the CRC-32 by default. */
    if (!mz_zip_reader_extract_to_mem(&a->mz, (mz_uint)idx, buf,
                                      (size_t)e->size, 0)) {
        free(buf);
        return (uint8_t *)zip_fail("zip: entry failed to decompress "
                                   "(corrupt data, or size/CRC mismatch)");
    }

    *out_len = e->size;
    return buf;
}
