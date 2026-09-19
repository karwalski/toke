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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Number of i64 slots in a $zipentry — keep in sync with stdlib/zip.tki. */
#define TK_ZIPENTRY_SLOTS 4

/* zip.open(data:@(byte)) -> $ziparchive!$ziperr
 *
 * Returns 0 on any rejection, which is what the compiled error-union ABI
 * lowers the `$err` arm to.  zip.lasterr() carries the reason. */
int64_t tk_zip_open_w(int64_t data)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    if (!buf) return 0;
    TkZipArchive *a = zip_open_mem(buf, n);
    free(buf);
    return (int64_t)(intptr_t)a;
}

/* zip.openfile(path:str) -> $ziparchive!$ziperr
 *
 * Reads the archive itself rather than going through std.file, which returns
 * a C string and therefore truncates at the first NUL — every zip has one
 * within its first handful of bytes.  Gated on fs.read like every other
 * filesystem sink (124.4c). */
int64_t tk_zip_openfile_w(int64_t path)
{
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    return (int64_t)(intptr_t)zip_open_file((const char *)(intptr_t)path);
}

/* zip.entries(a:$ziparchive) -> @($zipentry)
 *
 * An archive with no entries yields a zero-length array, never 0: a null
 * would be indexed by the caller. */
int64_t tk_zip_entries_w(int64_t handle)
{
    TkZipArchive *a = (TkZipArchive *)(intptr_t)handle;
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
         * array, so copy rather than alias. */
        size_t len = strlen(e->name);
        char *name = (char *)malloc(len + 1);
        if (!name) { free(slot); break; }
        memcpy(name, e->name, len + 1);

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
 * 0 on failure (the `$err` arm).  A genuinely empty entry returns a
 * zero-length array, which is a distinct value from 0. */
int64_t tk_zip_read_w(int64_t handle, int64_t name)
{
    TkZipArchive *a = (TkZipArchive *)(intptr_t)handle;
    if (!a || !name) return 0;
    uint64_t len = 0;
    uint8_t *buf = zip_read_entry(a, (const char *)(intptr_t)name, &len);
    if (!buf) return 0;
    int64_t out = tk_bytes_pack(buf, len);
    free(buf);
    return out;
}

/* zip.close(a:$ziparchive) -> void — releases the archive and its names. */
int64_t tk_zip_close_w(int64_t handle)
{
    zip_close((TkZipArchive *)(intptr_t)handle);
    return 0;
}

/* zip.lasterr() -> str — why the last zip call failed; "" if none has. */
int64_t tk_zip_lasterr_w(void)
{
    const char *m = zip_lasterr();
    return (int64_t)(intptr_t)(m ? m : "");
}
