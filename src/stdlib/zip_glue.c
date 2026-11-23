/* SPDX-License-Identifier: MIT */
/*
 * zip_glue.c — i64-ABI wrappers for the std.zip module (story 135.1).
 *
 * Registered against module "zip" in src/stdlib_deps.c.  136.33 is the reason
 * that sentence is written down: a glue file registered under the wrong module
 * name left six modules unlinkable on their own, and the failure only shows up
 * when a program imports THAT module and nothing else.  test/stdlib/zip.sh
 * compiles such a program as its first case.
 *
 * ABI notes (docs/runtime-abi.md):
 *   §3  a `str` is a plain NUL-terminated char*, cast through i64.
 *   §4  an array is a length-prefixed i64 block; the handle points at
 *       data[0] and the length sits at handle[-1] (tk_array.h).
 *       A `@(byte)` stores ONE BYTE PER i64 SLOT (bytes_rt.h) — hence
 *       tk_bytes_pack rather than handing over a raw buffer.
 *   §5  a struct is a flat i64 block, one slot per declared field, in
 *       declaration order.
 *
 * The §5 point is the one that bites.  A `$zipentry` built here MUST have
 * exactly the four slots stdlib/zip.tki declares, in that order:
 *
 *     slot 0  name            str
 *     slot 1  size            i64
 *     slot 2  compressedsize  i64
 *     slot 3  isdir           bool (0/1)
 *
 * If the shapes disagree the program still compiles and then reads whatever
 * sits at the offset — 127.86 read the first eight bytes of an id string as a
 * size and reported 49, 50, 51 for buffers of 128, 256 and 512 bytes.  There
 * is no diagnostic for that; the only defence is that the .tki and this file
 * are edited together, and that a test reads every field and checks the value.
 */

#include "zip.h"
#include "tk_array.h"
#include "bytes_rt.h"
#include "capabilities.h"
#include "tk_runtime.h"   /* 127.102c: tk_current_error + tk_err_box (127.109) */
#include "tk_strhdr.h"  /* 127.295: sealed strings, tk_strlen */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Number of i64 slots in a $zipentry — keep in sync with stdlib/zip.tki. */
#define TK_ZIPENTRY_SLOTS 4

/* ── The error channel (127.102c) ───────────────────────────────────────
 *
 * Every `T!$ziperr` wrapper keeps @tk_current_error truthful on EVERY return
 * (runtime-abi.md §7.2): a failure stores a {tag, msg} box of $ziperr's sum
 * layout (§7.3) — the tag is the variant index zip_lasterr_kind() names, the
 * msg is zip_lasterr()'s literal — and a success stores 0, so an earlier
 * failure can never leak into a later call's `mt` (127.123).  The `$err` arm
 * therefore binds the variant and its reason:
 *
 *     mt zip.read(a;"x") {$ok:b …; $err:e mt e {$notfound:m …; $toolarge:m …}}
 *
 * Until 127.102c these wrappers returned a bare 0, the arm bound nil, and the
 * reason was reachable only through zip.lasterr(), which is now retired.
 * The failure value is still 0: every T here is a handle or an array, and a
 * genuinely empty entry is a real zero-length array, never 0. */
static int64_t zip_raise(TkZipErrKind kind, const char *msg)
{
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(kind == ZIP_E_NONE ? ZIP_E_IO : kind);
    box[1] = (int64_t)(intptr_t)(msg && *msg ? msg : "zip: unknown error");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* The failure recorded by the C core, as the box. */
static int64_t zip_raise_last(void)
{
    return zip_raise(zip_lasterr_kind(), zip_lasterr());
}

static int64_t zip_ok(int64_t v)
{
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return v;
}

/* 138.45: a ziparchive is looked up in the runtime's handle registry
 * (tk_runtime.c) before it is read (the value toke holds is a registry
 * token, never the address, so a released one stays refused after malloc
 * reuses its memory).  Any value zip.open/openfile did not hand
 * out, or one zip.close has closed, is answered as the null archive is
 * (entries @(), read $err, close a no-op); it used to be read as a
 * TkZipArchive and the program died with SIGSEGV (zip.entries(5)), and a
 * second close freed the archive twice. */
static TkZipArchive *zip_in(int64_t h) { return (TkZipArchive *)tk_handle_token(TK_HK_ZIPARCHIVE, h); }
static int64_t zip_out(TkZipArchive *a)
{
    int64_t tok = tk_handle_issue(TK_HK_ZIPARCHIVE, a);   /* a token, not the address */
    if (!tok) {
        zip_close(a);
        return zip_raise(ZIP_E_IO, "zip: out of memory");
    }
    return zip_ok(tok);
}

/* zip.open(data:@(byte)) -> $ziparchive!$ziperr */
int64_t tk_zip_open_w(int64_t data)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    if (!buf) return zip_raise(ZIP_E_BADARCHIVE, "zip: empty archive");
    TkZipArchive *a = zip_open_mem(buf, n);
    free(buf);
    if (!a) return zip_raise_last();
    return zip_out(a);
}

/* zip.openfile(path:str) -> $ziparchive!$ziperr
 *
 * Reads the archive itself rather than going through std.file, which returns
 * a C string and therefore truncates at the first NUL — every zip has one
 * within its first handful of bytes.  Gated on fs.read like every other
 * filesystem sink (124.4c). */
int64_t tk_zip_openfile_w(int64_t path)
{
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);   /* 138.19 */
    if (!path) return zip_raise(ZIP_E_IO, "zip: empty path");
    TkZipArchive *a = zip_open_file((const char *)(intptr_t)path);
    if (!a) return zip_raise_last();
    return zip_out(a);
}

/* zip.entries(a:$ziparchive) -> @($zipentry)
 *
 * An archive with no entries yields a zero-length array, never 0: a null
 * would be indexed by the caller. */
int64_t tk_zip_entries_w(int64_t handle)
{
    TkZipArchive *a = zip_in(handle);
    uint32_t n = zip_entry_count(a);
    if (!a || n == 0) return tk_arr_alloc(0, 0);

    int64_t h = tk_arr_alloc((int64_t)n, (int64_t)n);
    if (!h) return tk_arr_alloc(0, 0);
    int64_t *block = (int64_t *)(intptr_t)h;

    uint32_t built = 0;
    for (uint32_t i = 0; i < n; i++) {
        const TkZipEntry *e = zip_entry_at(a, i);
        if (!e) break;
        int64_t *slot = (int64_t *)malloc(TK_ZIPENTRY_SLOTS * sizeof(int64_t));
        if (!slot) break;
        /* The archive owns its names and is closed independently of this
         * array, so copy rather than alias -- a sealed copy (127.295). */
        char *name = tks_strdup(e->name);
        if (!name) { free(slot); break; }

        slot[0] = (int64_t)(intptr_t)name;   /* .name           */
        slot[1] = (int64_t)e->size;          /* .size           */
        slot[2] = (int64_t)e->csize;         /* .compressedsize */
        slot[3] = e->is_dir ? 1 : 0;         /* .isdir          */
        block[built++] = (int64_t)(intptr_t)slot;
    }
    tk_arr_setlen(h, (int64_t)built);
    return h;
}

/* zip.read(a:$ziparchive; name:str) -> @(byte)!$ziperr
 *
 * A genuinely empty entry returns a zero-length array, which is a distinct
 * value from the failure filler 0 — and the slot, not the value, decides. */
int64_t tk_zip_read_w(int64_t handle, int64_t name)
{
    TkZipArchive *a = zip_in(handle);
    if (!a || !name) return zip_raise(ZIP_E_IO, "zip: invalid argument");
    uint64_t len = 0;
    uint8_t *buf = zip_read_entry(a, (const char *)(intptr_t)name, &len);
    if (!buf) return zip_raise_last();
    int64_t out = tk_bytes_pack(buf, len);
    free(buf);
    if (!out) return zip_raise(ZIP_E_IO, "zip: out of memory");
    return zip_ok(out);
}

/* zip.close(a:$ziparchive) -> void — releases the archive and its names. */
int64_t tk_zip_close_w(int64_t handle)
{
    TkZipArchive *a = zip_in(handle);   /* 138.45: closed once; a second close is refused */
    if (a) { tk_handle_drop(a); zip_close(a); }
    return 0;
}
