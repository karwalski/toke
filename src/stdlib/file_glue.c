/*
 * file_glue.c — i64-ABI wrappers for std.file module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.file.
 */

#include "file.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "capabilities.h"   /* 124.4c: fs.read / fs.write capability gates */
#include "bytes_rt.h"       /* 135.10: @(byte) <-> contiguous uint8_t buffer */
#include "tk_runtime.h"     /* 135.12: tk_current_error + tk_err_box (127.109) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ══ The error channel (135.12, then 127.102b for the whole module) ═══════
 *
 * Every `T!FileErr` wrapper in this file keeps @tk_current_error truthful on
 * EVERY return (runtime-abi.md §7.2): a failure stores a {tag, msg} box of
 * FileErr's sum layout (§7.3) and a success stores 0, so the compiler — which
 * derives these as slot wrappers from stdlib/file.tki — decides ok/err on the
 * slot and the $err arm binds the variant and its reason:
 *
 *     mt file.read(p) {$ok:t …; $err:e mt e {$notfound:m …; $isdir:m …}}
 *
 * THE TAG IS FileErrKind ITSELF.  stdlib/file.tki declares FileErr's ten
 * variants in exactly the enum's order (file.h), which is what lets the
 * accessor file.lasterrkind() retire without losing a case: until 127.102b
 * FileErr carried three variants while file.c distinguished ten, so
 * "is a directory", "is a symlink" and "over the cap" were reachable only
 * through the accessor.  Keep the enum and the .tki in step.
 *
 * THE MESSAGE OUTLIVES THE NEXT CALL.  file.c formats its messages into one
 * static buffer that the next failure overwrites, but a `let`-bound error
 * keeps a copy of its BOX (127.103), and the box holds a pointer — so every
 * raise copies its message through tk_err_msg() (tk_runtime.c), whose
 * per-thread ring keeps it exact across the next seven raises, with no
 * allocation per error (127.109). */
/* The box: slot 0 = the FileErr variant index (== FileErrKind), slot 1 = the
 * message.  tk_err_box never returns null (a null would be stored as 0, and 0
 * is success), so there is no failure path to write here. */
static int64_t file_raise(FileErrKind kind, const char *msg) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)kind;
    box[1] = (int64_t)(intptr_t)tk_err_msg("%s", msg ? msg : "");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* A failure reported by a FileResult, named for the call and the path —
 * file.c's errno messages ("file not found") name neither. */
static int64_t file_raise_op(const char *op, FileErr e, int64_t path) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)e.kind;
    box[1] = (int64_t)(intptr_t)tk_err_msg("%s: %s: %s", op,
                                         e.msg ? e.msg : "I/O error",
                                         path ? (const char *)(intptr_t)path : "");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

static int64_t file_badarg(const char *msg) {
    file_setlasterr(FILE_ERR_BAD_ARG, msg);
    return file_raise(FILE_ERR_BAD_ARG, msg);
}

static int64_t file_ok(int64_t v) {
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return v;
}

/* A [str] from a StrArray, or a nomem raise. */
static int64_t file_strarray(const char *op, StrArray arr, int free_data) {
    int64_t h = tk_arr_alloc((int64_t)arr.len, (int64_t)arr.len);
    if (!h) {
        if (free_data) free(arr.data);
        return file_raise(FILE_ERR_NO_MEM,
                          tk_err_msg("%s: array allocation failed", op));
    }
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i] = (int64_t)(intptr_t)arr.data[i];
    if (free_data) free(arr.data);
    return file_ok(h);
}

int64_t tk_file_read_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return file_badarg("file.read: null path");
    StrFileResult r = file_read((const char *)(intptr_t)path);
    if (r.is_err) return file_raise_op("file.read", r.err, path);
    return file_ok((int64_t)(intptr_t)r.ok);
}

int64_t tk_file_write_w(int64_t path, int64_t content) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path) return file_badarg("file.write: null path");
    if (!content) return file_badarg("file.write: null content");
    BoolFileResult r = file_write((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    if (r.is_err) return file_raise_op("file.write", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_isdir_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    return (int64_t)file_is_dir((const char *)(intptr_t)path);
}

int64_t tk_file_mkdir_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path) return file_badarg("file.mkdir: null path");
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    if (r.is_err) return file_raise_op("file.mkdir", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_copy_w(int64_t src, int64_t dst) {
    TK_REQUIRE(TK_CAP_FS_READ);   /* reads src */
    TK_REQUIRE(TK_CAP_FS_WRITE);  /* writes dst */
    if (!src || !dst) return file_badarg("file.copy: null path");
    BoolFileResult r = file_copy((const char *)(intptr_t)src, (const char *)(intptr_t)dst);
    if (r.is_err) return file_raise_op("file.copy", r.err, src);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_listall_w(int64_t dir) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!dir) return file_badarg("file.listall: null path");
    StrArrayFileResult r = file_listall((const char *)(intptr_t)dir);
    if (r.is_err) return file_raise_op("file.listall", r.err, dir);
    /* The strings AND the pointer block are handed over as before: listall's
     * data block was never freed here, and freeing it now is not this
     * story's change. */
    return file_strarray("file.listall", r.ok, 0);
}

int64_t tk_file_exists_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    return (int64_t)file_exists((const char *)(intptr_t)path);
}

/* ── wrappers for additional file operations ────────────────────────────── */

int64_t tk_file_list_w(int64_t dir) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!dir) return file_badarg("file.list: null path");
    StrArrayFileResult r = file_list((const char *)(intptr_t)dir);
    if (r.is_err) return file_raise_op("file.list", r.err, dir);
    return file_strarray("file.list", r.ok, 1);
}

/*
 * Story 136.23 — file.append(path; content).
 *
 * This wrapper carried a third parameter it discarded with `(void)extra;`.
 * stdlib/file.tki, docs/stdlib/file.md and file_append() in file.c have all
 * only ever had two, nothing in the tree ever passed a third, and the surplus
 * parameter was the sole failure in `make check-patterns`: the io-write-
 * accumulate catalogue entry calls file.append with the documented two.
 */
int64_t tk_file_append_w(int64_t path, int64_t content) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path) return file_badarg("file.append: null path");
    if (!content) return file_badarg("file.append: null content");
    BoolFileResult r = file_append((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    if (r.is_err) return file_raise_op("file.append", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_delete_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path) return file_badarg("file.delete: null path");
    BoolFileResult r = file_delete((const char *)(intptr_t)path);
    if (r.is_err) return file_raise_op("file.delete", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_rename_w(int64_t from, int64_t to) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!from || !to) return 0;
    BoolFileResult r = file_move((const char *)(intptr_t)from, (const char *)(intptr_t)to);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_readlines_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    StrArrayFileResult r = file_readlines((const char *)(intptr_t)path);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t h = tk_arr_alloc((int64_t)arr.len, (int64_t)arr.len);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i] = (int64_t)(intptr_t)arr.data[i];
    free(arr.data);
    return h;
}

int64_t tk_file_writelines_w(int64_t path, int64_t lines) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path || !lines) return 0;
    /* lines points to block+1; block[0] (at lines[-1]) is the length */
    int64_t *arr = (int64_t *)(intptr_t)lines;
    int64_t len = arr[-1];
    if (len <= 0) {
        /* Empty array: write empty string */
        BoolFileResult r = file_write((const char *)(intptr_t)path, "");
        return r.is_err ? 0 : (int64_t)r.ok;
    }
    /* Calculate total size needed */
    size_t total = 0;
    for (int64_t i = 0; i < len; i++) {
        const char *line = (const char *)(intptr_t)arr[i];
        if (line) total += strlen(line);
        total++; /* for '\n' */
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) return 0;
    size_t pos = 0;
    for (int64_t i = 0; i < len; i++) {
        const char *line = (const char *)(intptr_t)arr[i];
        if (line) {
            size_t slen = strlen(line);
            memcpy(buf + pos, line, slen);
            pos += slen;
        }
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    BoolFileResult r = file_write((const char *)(intptr_t)path, buf);
    free(buf);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/*
 * file.stat(path) — the size, through the same file_size() that file.size
 * uses.  135.12 replaced that function's 28.2-era stat(2) body rather than
 * adding a second one, so this call changed with it and in the same
 * direction: it no longer follows a symlink whose target the read calls would
 * refuse, and it no longer reports st_size for a DIRECTORY or a FIFO, where
 * the number was never a count of readable bytes.  Those now return 0 here.
 *
 * file.stat keeps the value sentinel, and file.size does not.  One C answer,
 * two error channels, which is 127.102b's business and not this story's; use
 * file.size, which can tell an empty file from a failure.
 */
int64_t tk_file_stat_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    U64FileResult r = file_size((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_err_w(int64_t msg) {
    /* Error accessor: return the message string as-is */
    return msg;
}

int64_t tk_file_listdir_w(int64_t path) {
    return tk_file_list_w(path);
}

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* file.appendline(path, line) — append a line with trailing newline */
int64_t tk_file_appendline_w(int64_t path, int64_t line) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path || !line) return 0;
    const char *l = (const char *)(intptr_t)line;
    size_t llen = strlen(l);
    char *with_nl = (char *)malloc(llen + 2);
    if (!with_nl) return 0;
    memcpy(with_nl, l, llen);
    with_nl[llen] = '\n';
    with_nl[llen + 1] = '\0';
    BoolFileResult r = file_append((const char *)(intptr_t)path, with_nl);
    free(with_nl);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/* file.ensuredir(path) — create directory if it doesn't exist (mkdir -p) */
int64_t tk_file_ensuredir_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path) return 0;
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/* file.listglob(pattern) — list files matching a glob pattern */
int64_t tk_file_listglob_w(int64_t pattern) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!pattern) return 0;
    StrArrayFileResult r = file_glob((const char *)(intptr_t)pattern);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t h = tk_arr_alloc((int64_t)arr.len, (int64_t)arr.len);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i] = (int64_t)(intptr_t)arr.data[i];
    free(arr.data);
    return h;
}

/* file.parsetoml(path) — read file and parse as TOML, return raw string
 * (delegates actual parsing to the toml module; here just reads the file) */
int64_t tk_file_parsetoml_w(int64_t path) {
    return tk_file_read_w(path);
}

/* file.remove(path) — alias for file.delete */
int64_t tk_file_remove_w(int64_t path) {
    return tk_file_delete_w(path);
}

/* 136.45: the std.fs aliases (tk_fs_read_w / tk_fs_write_w / tk_fs_writetext_w,
 * a second name for std.file that one downstream project imported) are
 * withdrawn with the module. */

/* file.tempdir() — return a temporary directory path */
int64_t tk_file_tempdir_w(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    return (int64_t)(intptr_t)tmp;
}


/* ══ 135.10 — binary file access ═════════════════════════════════════════
 *
 * ABI (docs/runtime-abi.md): a `@(byte)` is an i64 array with ONE BYTE PER
 * I64 SLOT (bytes_rt.h), so the buffer is packed rather than handed over.
 *
 * 0 is NOT reachable for a success: tk_bytes_pack of an empty buffer returns
 * a real zero-length array handle, so "the file is empty" and "the file could
 * not be read" are different values — the whole point of the story.  Since
 * 127.102b the slot, not the value, is what the compiler decides on, and the
 * $err arm binds which of the nine outcomes it was.
 */

int64_t tk_file_readbytes_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return file_badarg("file.readbytes: null path");
    BytesFileResult r = file_readbytes((const char *)(intptr_t)path);
    if (r.is_err) return file_raise(r.err.kind, r.err.msg);

    int64_t h = tk_bytes_pack(r.ok.data, r.ok.len);
    free(r.ok.data);
    if (!h) {
        /* tk_arr_alloc failed: 8 bytes per byte, so this is reachable for a
         * file that passed the cap.  Do not return an empty array for it. */
        file_setlasterr(FILE_ERR_NO_MEM,
                        "file.readbytes: byte array allocation failed");
        return file_raise(FILE_ERR_NO_MEM, file_lasterr());
    }
    return file_ok(h);
}

int64_t tk_file_writebytes_w(int64_t path, int64_t data) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!path) return file_badarg("file.writebytes: null path");
    if (!data) {
        /* A genuinely empty `@(byte)` is a real zero-length handle, never 0.
         * A 0 here is an unchecked error value from upstream, and writing an
         * empty file for it would turn someone else's failure into a
         * successful-looking truncation. */
        return file_badarg("file.writebytes: null byte array (an unchecked error value?)");
    }
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    if (!buf) {
        file_setlasterr(FILE_ERR_NO_MEM, "file.writebytes: staging buffer allocation failed");
        return file_raise(FILE_ERR_NO_MEM, file_lasterr());
    }
    BoolFileResult r = file_writebytes((const char *)(intptr_t)path, buf, n);
    free(buf);
    if (r.is_err) return file_raise(r.err.kind, r.err.msg);
    return file_ok((int64_t)r.ok);
}

/* ══ 135.12 — the bounded read and the size ══════════════════════════════
 *
 * These two were the first in this module to store a real box instead of a
 * value sentinel, because for file.size the sentinel is WRONG, not merely
 * weak: an EMPTY FILE HAS SIZE 0, and 0 is what the sentinel convention means
 * by "it failed" (runtime-abi.md §7.2: "the zero filler is not the
 * discriminant").  127.102b gave the rest of the module the same shape and
 * retired file.lasterr() / file.lasterrkind(); these two are unchanged.
 */

/* file.readrange(path; offset; len) -> @(byte)!FileErr */
int64_t tk_file_readrange_w(int64_t path, int64_t offset, int64_t len) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return file_badarg("file.readrange: null path");
    BytesFileResult r = file_readrange((const char *)(intptr_t)path, offset, len);
    if (r.is_err) return file_raise(r.err.kind, r.err.msg);

    int64_t h = tk_bytes_pack(r.ok.data, r.ok.len);
    free(r.ok.data);
    if (!h) {
        /* tk_arr_alloc failed: 8 bytes per byte, so this is reachable for a
         * window that passed the cap.  Do not return an empty array for it. */
        file_setlasterr(FILE_ERR_NO_MEM,
                        "file.readrange: byte array allocation failed");
        return file_raise(FILE_ERR_NO_MEM, file_lasterr());
    }
    return file_ok(h);
}

/* file.size(path) -> i64!FileErr — 0 is a real size, never a failure. */
int64_t tk_file_size_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return file_badarg("file.size: null path");
    U64FileResult r = file_size((const char *)(intptr_t)path);
    if (r.is_err) return file_raise(r.err.kind, r.err.msg);
    return file_ok((int64_t)r.ok);
}
