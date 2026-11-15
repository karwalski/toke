/* SPDX-License-Identifier: MIT */
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
#include "tk_strhdr.h"      /* 127.170: file.read returns a sealed string */
#include <stdint.h>
#include <stdio.h>
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

/* ══ 127.295 (S1 step 5, thirteenth tranche) — every str handed to toke is sealed
 *
 * file.c's producers (file_realpath, file_list, file_listall, file_readlines,
 * file_glob) stay malloc'd: their C callers -- toml.c (file_read),
 * test_file.c, C592 and the coverage test -- free what they return.  So the seal is made here, at
 * the boundary: seal_take() copies a result into a sealed string and frees
 * the malloc'd original, handing the original over unsealed only if the copy
 * cannot be made (an unsealed string is read correctly, only with strlen).
 * file.read was already sealed at the source (file_read_into with
 * tks_alloc/tks_seal, 127.170).  A sealed string is never freed, by anyone.
 *
 * Left alone, on purpose: file.err returns its argument; the "/tmp" fallback
 * of file.tempdir is a literal. */
static const char *seal_take(const char *s) {
    if (!s) return NULL;
    char *t = tks_strdup(s);
    if (!t) return s;
    free((void *)s);
    return t;
}

/* Seal every element of a StrArray in place (each original freed). */
static void seal_all(StrArray arr) {
    for (uint64_t i = 0; i < arr.len; i++)
        arr.data[i] = seal_take(arr.data[i]);
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
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.read: null path");
    StrFileResult r = file_read_into((const char *)(intptr_t)path, tks_alloc, tks_seal);
    if (r.is_err) return file_raise_op("file.read", r.err, path);
    return file_ok((int64_t)(intptr_t)r.ok);
}

int64_t tk_file_write_w(int64_t path, int64_t content) {
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.write: null path");
    if (!content) return file_badarg("file.write: null content");
    BoolFileResult r = file_write((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    if (r.is_err) return file_raise_op("file.write", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_isdir_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
    if (!path) return 0;
    return (int64_t)file_is_dir((const char *)(intptr_t)path);
}

int64_t tk_file_mkdir_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.mkdir: null path");
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    if (r.is_err) return file_raise_op("file.mkdir", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_copy_w(int64_t src, int64_t dst) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)src);    /* reads src */
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)dst);   /* writes dst */
    if (!src || !dst) return file_badarg("file.copy: null path");
    /* 121.84: name the path that failed -- dst for an open/write/close of
     * the destination, src otherwise (it used to be src for every error). */
    int dst_failed = 0;
    BoolFileResult r = file_copy_at((const char *)(intptr_t)src,
                                    (const char *)(intptr_t)dst, &dst_failed);
    if (r.is_err) return file_raise_op("file.copy", r.err, dst_failed ? dst : src);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_listall_w(int64_t dir) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)dir);
    if (!dir) return file_badarg("file.listall: null path");
    StrArrayFileResult r = file_listall((const char *)(intptr_t)dir);
    if (r.is_err) return file_raise_op("file.listall", r.err, dir);
    seal_all(r.ok);
    /* 127.415: the pointer block is freed, as file.list's is.  It used to be
     * handed over (free_data=0) and leaked on every call: file_listall's
     * thread-local accumulator is replaced by the next call, never freed.
     * Every element is a sealed copy now, so after the copy into the toke
     * array nothing reads the block. */
    return file_strarray("file.listall", r.ok, 1);
}

int64_t tk_file_exists_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
    if (!path) return 0;
    return (int64_t)file_exists((const char *)(intptr_t)path);
}

/* ── wrappers for additional file operations ────────────────────────────── */

/* 121.60: file.realpath(path):$str!$fileerr.  Resolving a path reads the
 * directories on it (lstat/readlink of every component), so it is fs.read,
 * like file.exists and file.isdir. */
int64_t tk_file_realpath_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.realpath: null path");
    StrFileResult r = file_realpath((const char *)(intptr_t)path);
    if (r.is_err) return file_raise_op("file.realpath", r.err, path);
    return file_ok((int64_t)(intptr_t)seal_take(r.ok));
}

int64_t tk_file_list_w(int64_t dir) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)dir);
    if (!dir) return file_badarg("file.list: null path");
    StrArrayFileResult r = file_list((const char *)(intptr_t)dir);
    if (r.is_err) return file_raise_op("file.list", r.err, dir);
    seal_all(r.ok);
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
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.append: null path");
    if (!content) return file_badarg("file.append: null content");
    BoolFileResult r = file_append((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    if (r.is_err) return file_raise_op("file.append", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_delete_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.delete: null path");
    BoolFileResult r = file_delete((const char *)(intptr_t)path);
    if (r.is_err) return file_raise_op("file.delete", r.err, path);
    return file_ok((int64_t)r.ok);
}

int64_t tk_file_rename_w(int64_t from, int64_t to) {
    /* 138.19: a move removes `from` and creates `to` -- both are writes. */
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)from);
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)to);
    if (!from || !to) return 0;
    BoolFileResult r = file_move((const char *)(intptr_t)from, (const char *)(intptr_t)to);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_readlines_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
    if (!path) return 0;
    StrArrayFileResult r = file_readlines((const char *)(intptr_t)path);
    if (r.is_err) return 0;
    seal_all(r.ok);
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
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
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
        if (line) total += tk_strlen(line);
        total++; /* for '\n' */
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) return 0;
    size_t pos = 0;
    for (int64_t i = 0; i < len; i++) {
        const char *line = (const char *)(intptr_t)arr[i];
        if (line) {
            size_t slen = tk_strlen(line);
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
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
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
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
    if (!path || !line) return 0;
    const char *l = (const char *)(intptr_t)line;
    size_t llen = tk_strlen(l);
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
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
    if (!path) return 0;
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/* 138.19: the directory a glob pattern lists -- its components before the
 * first one holding a glob metacharacter ("content/x*.md" -> "content"; a
 * pattern with none is checked as itself).  A ".." among the globbed
 * components could climb out of any directory named here, so such a pattern
 * is checked as "/": only the whole class (or a "/" scope) admits it. */
static const char *glob_scope_dir(const char *pattern, char *out, size_t outsz) {
    if (!pattern) return NULL;
    size_t n = strlen(pattern), meta = n, cut = 0;
    for (size_t i = 0; i < n; i++)
        if (strchr("*?[{", pattern[i])) { meta = i; break; }
    if (meta == n) { snprintf(out, outsz, "%s", pattern); return out; }
    for (size_t i = meta; i + 1 < n; i++)          /* a ".." component after the glob */
        if (pattern[i] == '.' && pattern[i + 1] == '.' &&
            (i == 0 || pattern[i - 1] == '/') && (i + 2 == n || pattern[i + 2] == '/')) {
            snprintf(out, outsz, "/");
            return out;
        }
    for (size_t i = 0; i < meta; i++) if (pattern[i] == '/') cut = i + 1;
    if (cut == 0) snprintf(out, outsz, ".");
    else if (cut == 1) snprintf(out, outsz, "/");
    else snprintf(out, outsz, "%.*s", (int)(cut - 1), pattern);
    return out;
}

/* file.listglob(pattern) — list files matching a glob pattern */
int64_t tk_file_listglob_w(int64_t pattern) {
    char gdir[4096];
    TK_REQUIRE_PATH(TK_CAP_FS_READ, glob_scope_dir((const char *)(intptr_t)pattern, gdir, sizeof gdir));
    if (!pattern) return 0;
    StrArrayFileResult r = file_glob((const char *)(intptr_t)pattern);
    if (r.is_err) return 0;
    seal_all(r.ok);
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

/* file.tempdir() — return a temporary directory path.
 * 127.295: a sealed copy, not getenv()'s pointer into the process
 * environment, which a later setenv/unsetenv of TMPDIR may overwrite or free
 * under a str the program still holds (the os.getenv / env.get defect of
 * 121.75 / 121.78).  If the copy cannot be made the environment's own
 * pointer is handed over, as before; "/tmp" is a literal. */
int64_t tk_file_tempdir_w(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp) return (int64_t)(intptr_t)"/tmp";
    char *t = tks_strdup(tmp);
    return (int64_t)(intptr_t)(t ? t : tmp);
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
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
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
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, (const char *)(intptr_t)path);
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
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
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
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);
    if (!path) return file_badarg("file.size: null path");
    U64FileResult r = file_size((const char *)(intptr_t)path);
    if (r.is_err) return file_raise(r.err.kind, r.err.msg);
    return file_ok((int64_t)r.ok);
}
