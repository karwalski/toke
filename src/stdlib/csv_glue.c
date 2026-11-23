/* SPDX-License-Identifier: MIT */
/*
 * csv_glue.c — i64-ABI wrappers for std.csv module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.csv.
 */

#include "csv.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "bytes_rt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tk_runtime.h"   /* 127.238: tk_current_error + tk_err_box */
#include "tk_strhdr.h"    /* 127.295: sealed strings, tk_strlen */

/* 127.295: every str this file hands toke is a sealed copy.  csv.c's fields
 * stay malloc'd because its C callers (test_csv.c, csv_reader_free for the
 * cached header) free them; seal_take copies a field the glue owns and frees
 * the original (handing the original over unsealed only if the copy cannot
 * be made), seal_copy copies a string something else still owns. */
static const char *seal_take(const char *s) {
    if (!s) return NULL;
    char *t = tks_strdup(s);
    if (!t) return s;
    free((void *)s);
    return t;
}
static const char *seal_copy(const char *s) {
    if (!s) return NULL;
    char *t = tks_strdup(s);
    return t ? t : "";
}

/* 127.238: the error slot (runtime-abi.md §7.2).  These `T!csverr` wrappers
 * returned a bare 0 on failure, so only the value sentinel saw it and the
 * $err arm bound nil (`e.msg` was RT005).  The failure value is unchanged
 * (0; every T here is a handle or pointer); every return goes through
 * csv_slot / csv_slot_core, which on 0 store a csverr record and on anything
 * else clear the slot, so the wrapper is a slot wrapper (llvm.c derives them
 * from the .tki).
 *
 * 127.102a: THE RECORD CARRIES WHAT THE ACCESSORS DID, so they could retire.
 * stdlib/csv.tki declares csverr {msg:str; line:u64; kind:str} and the box
 * is exactly those three slots, in that order (runtime-abi.md §7.3):
 *
 *     slot 0  msg   the core's message (it names the line), else the
 *                   wrapper's own sentence            — was csv.lasterr()
 *     slot 1  line  1-based physical line, 0 if none  — was csv.lasterrline()
 *     slot 2  kind  the stable token: eof opts empty bom encoding delimiter
 *                   quote ragged, plus badarg / nomem for a failure in
 *                   this file                         — was csv.lasterrkind()
 *
 * `line` was always declared and always 0 until now; `kind` is new.  The
 * message is copied out of csv.c's single static buffer through tk_err_msg()
 * (tk_runtime.c), because a `let`-kept error keeps a copy of its BOX
 * (127.103) and the box holds a pointer: without the copy the next csv call
 * would rewrite a message the caller still holds.  No allocation per error
 * (127.109). */
static int64_t csv_raise(const char *msg, uint64_t line, const char *kind) {
    const char *m = tk_err_msg("%s", msg ? msg : "");
    int64_t *box = (int64_t *)tk_err_box(3 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)m;
    box[1] = (int64_t)line;
    box[2] = (int64_t)(intptr_t)(kind ? kind : "empty");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* A wrapper whose C call does not record a reason (csv_parse): the
 * wrapper's own sentence and kind. */
static int64_t csv_slot(int64_t v, const char *msg, const char *kind) {
    if (v) { tk_current_error = 0; return v; }
    return csv_raise(msg, 0, kind);
}

/* A wrapper whose C call records its reason in csv.c's last-error state
 * (the hardened reader, csv_sniff): the core's message, line and kind.  The
 * core leaves the message empty for end of data (it is not a refusal) and
 * the kind "ok" only on a path that records nothing, so both fall back to the
 * wrapper's own sentence. */
static int64_t csv_slot_core(int64_t v, const char *msg, const char *kind) {
    if (v) { tk_current_error = 0; return v; }
    const char *k = csv_lasterrkind();
    const char *m = csv_lasterr();
    if (!k || !strcmp(k, "ok")) return csv_raise(msg, 0, kind);
    return csv_raise(m && *m ? m : msg, csv_lasterrline(), k);
}

/*
 * tk_csv_parse_w — parse a CSV string and return a pointer to an array of
 * StrArrays (one per row).  The toke runtime receives this as an opaque
 * pointer that it can iterate.  Returns 0 on empty/NULL input.
 *
 * Layout returned: heap block where [0] = nrows (as int64_t), followed by
 * nrows StrArray structs.  The caller (toke runtime) knows this layout.
 */
static const char tk_csv_parse_fail[] =
    "csv.parse: no rows (the input is empty or could not be parsed)";
int64_t tk_csv_parse_w(int64_t data) {
    if (!data) return csv_slot(0, tk_csv_parse_fail, "empty");
    /* The argument is a toke [byte] (packed), not a C string — unpack it. */
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    /* 127.422: the unpacked copy of the input leaked on every call. */
    if (!bytes || len == 0) { free(bytes); return csv_slot(0, tk_csv_parse_fail, "empty"); }
    char *s = (char *)malloc(len + 1);
    if (!s) { free(bytes); return csv_slot(0, tk_csv_parse_fail, "empty"); }
    memcpy(s, bytes, len); s[len] = '\0';
    free(bytes);
    uint64_t nrows = 0;
    StrArray *rows = csv_parse(s, len, &nrows);
    free(s);
    if (!rows || nrows == 0) { if (rows) free(rows); return csv_slot(0, tk_csv_parse_fail, "empty"); }
    /* Return a toke-native [csvrow]: outer[-1]=nrows, outer[i]=*csvrow, where
     * a csvrow is a 1-field struct {fields:[str]} and [str] is the toke array
     * convention (block[-1]=len, block[i]=char*). */
    int64_t outer_h = tk_arr_alloc((int64_t)nrows, (int64_t)nrows);
    if (!outer_h) { free(rows); return csv_slot(0, tk_csv_parse_fail, "empty"); }
    int64_t *outer = (int64_t *)(intptr_t)outer_h;
    for (uint64_t i = 0; i < nrows; i++) {
        uint64_t fl = rows[i].len;
        int64_t fields_h = tk_arr_alloc((int64_t)fl, (int64_t)fl);
        if (!fields_h) { outer[i] = 0; continue; }
        int64_t *fields = (int64_t *)(intptr_t)fields_h;
        for (uint64_t j = 0; j < fl; j++)
            fields[j] = (int64_t)(intptr_t)seal_take(rows[i].data[j]);
        free((void *)rows[i].data);   /* 127.422: the row's pointer block leaked */
        int64_t *cr = (int64_t *)malloc(sizeof(int64_t));
        if (!cr) {   /* 135.3c: the fields block leaked here */
            free((int64_t *)(intptr_t)fields_h - TK_ARR_HDR);
            outer[i] = 0; continue;
        }
        cr[0] = fields_h;
        outer[i] = (int64_t)(intptr_t)cr;
    }
    free(rows);
    return csv_slot(outer_h, tk_csv_parse_fail, "empty");
}

/*
 * csv.serialize(rows: [[str]]) -> str (127.354).  rows is a toke array of
 * rows, each a toke [str] (runtime-abi.md §4: the handle points at data[0],
 * the length sits at handle[-1], each slot is one element).  The rows are
 * written as csv.writer(44) + csv.writerow per row + csv.flush would write
 * them -- comma-separated, RFC 4180 quoting, CRLF between rows, no trailing
 * line break -- and returned as one sealed str.  No rows (or an absent
 * handle) is "".  It used to read a packed [nrows][StrArray...] C block that
 * no toke value has, typed i64 in csv.tki, so it was unusable from toke.
 */
int64_t tk_csv_serialize_w(int64_t rows) {
    int64_t *outer = (int64_t *)(intptr_t)rows;
    int64_t nrows = outer ? outer[-1] : 0;
    TkCsvWriter *w = csv_writer_new();
    if (!w) return (int64_t)(intptr_t)seal_copy("");
    for (int64_t i = 0; i < nrows; i++) {
        int64_t *row = (int64_t *)(intptr_t)outer[i];
        StrArray a;
        a.len = (uint64_t)(row && row[-1] > 0 ? row[-1] : 0);
        a.data = (const char **)row;
        csv_writer_writerow(w, a);
    }
    const char *result = csv_writer_flush(w);
    csv_writer_free(w);
    if (!result) return (int64_t)(intptr_t)seal_copy("");
    return (int64_t)(intptr_t)seal_take(result);
}

/* ── 114.43: streaming reader/writer wrappers ────────────────────────────
 * The streaming API was declared in csv.tki but had no `_w` wrappers, so any
 * program using it failed to link. csv.c already implements the primitives. */

/* Build a toke [str] (block[-1]=len, block[i]=char*) from a StrArray.
 * 127.295: each element is sealed -- taken (the row's fields belong to the
 * caller) or copied (the header's fields belong to the reader, which
 * csv.close frees while the program may still hold the array). */
static int64_t strarray_to_tokearr(StrArray a, int take) {
    int64_t h = tk_arr_alloc((int64_t)a.len, (int64_t)a.len);
    if (!h) return 0;
    int64_t *blk = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < a.len; i++)
        blk[i] = (int64_t)(intptr_t)(take ? seal_take(a.data[i]) : seal_copy(a.data[i]));
    return h;
}

/* 138.63: the registry (tk_runtime.h) vouches for every reader and writer
 * a toke program holds.  A $csvreader has a release call (csv.close), so it
 * is a TOKEN: a closed reader is refused even after malloc hands its memory
 * to a newer one.  A $csvwriter is never released from toke, so it is its
 * address.  A value the runtime did not hand out -- `5 as $csvreader`, a
 * record literal, a closed reader -- is answered as the null reader/writer
 * and never read. */
static TkCsvReader *csvr_in(int64_t h) { return (TkCsvReader *)tk_handle_token(TK_HK_CSVREADER, h); }
static TkCsvWriter *csvw_in(int64_t h) { return (TkCsvWriter *)tk_handle_addr(TK_HK_CSVWRITER, h); }
/* r as a toke $csvreader (0, with r freed, when the table is full). */
static int64_t csvr_out(TkCsvReader *r)
{
    if (!r) return 0;
    int64_t tok = tk_handle_issue(TK_HK_CSVREADER, r);
    if (!tok) csv_reader_free(r);
    return tok;
}

/* csv.reader([byte] data; u8 sep) -> csvreader. csv_reader_new references the
 * data buffer, so it is kept alive for the reader's lifetime. */
int64_t tk_csv_reader_w(int64_t data, int64_t sep) {
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    char *s = (char *)malloc(len + 1);
    if (!s) { free(bytes); return 0; }
    if (bytes && len) memcpy(s, bytes, len);
    s[len] = '\0';
    free(bytes);   /* 127.422: the unpacked copy leaked; the reader adopts s */
    TkCsvReader *r = csv_reader_new(s, len);
    if (!r) { free(s); return 0; }
    csv_reader_adopt(r, s);   /* 135.3c: freed by csv.close, not leaked */
    if (sep) csv_reader_set_separator(r, (char)sep);
    return csvr_out(r);
}

/* csv.next(csvreader) -> csvrow!csverr — 0 (err sentinel) at end of data. */
static const char tk_csv_next_fail[] =
    "csv.next: no more rows (end of input, or a row could not be read)";
int64_t tk_csv_next_w(int64_t reader) {
    TkCsvReader *r = csvr_in(reader);
    if (!r) return csv_slot(0, "csv.next: null reader (an unchecked error value?)", "badarg");
    /* 135.2: no has_next() short-circuit.  Going through csv_reader_next is
     * what sets the last-error kind, so "eof" and a refusal stop being the
     * same silent 0 to the caller. */
    StrArray row = csv_reader_next(r);
    if (!row.data || row.len == 0) return csv_slot_core(0, tk_csv_next_fail, "eof");
    int64_t fields = strarray_to_tokearr(row, 1);
    if (fields) free((void *)row.data);   /* 127.422: the row's pointer block leaked */
    int64_t *cr = (int64_t *)malloc(sizeof(int64_t));   /* csvrow = {fields:[str]} */
    if (!cr) return csv_slot(0, "csv.next: out of memory", "nomem");
    cr[0] = fields;
    return csv_slot_core((int64_t)(intptr_t)cr, tk_csv_next_fail, "eof");
}

/* csv.header(csvreader) -> [str]!csverr */
static const char tk_csv_header_fail[] =
    "csv.header: the reader has no header row";
int64_t tk_csv_header_w(int64_t reader) {
    TkCsvReader *r = csvr_in(reader);
    if (!r) return csv_slot(0, "csv.header: null reader (an unchecked error value?)", "badarg");
    StrArray h = csv_reader_header(r);
    if (!h.data) return csv_slot_core(0, tk_csv_header_fail, "eof");
    return csv_slot_core(strarray_to_tokearr(h, 0), tk_csv_header_fail, "eof");
}

/* csv.writer(u8 sep) -> csvwriter */
int64_t tk_csv_writer_w(int64_t sep) {
    TkCsvWriter *w = csv_writer_new();
    if (!w) return 0;
    if (sep) csv_writer_set_separator(w, (char)sep);
    if (!tk_handle_adopt(TK_HK_CSVWRITER, w)) { csv_writer_free(w); return 0; }
    return (int64_t)(intptr_t)w;
}

/* csv.writerow(csvwriter; [str] row) -> void */
int64_t tk_csv_writerow_w(int64_t writer, int64_t row) {
    TkCsvWriter *w = csvw_in(writer);
    if (!w || !row) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)row;
    int64_t n = ptr[-1];
    StrArray a;
    a.len = (uint64_t)(n > 0 ? n : 0);
    a.data = (const char **)ptr;
    csv_writer_writerow(w, a);
    return 0;
}

/* csv.flush(csvwriter) -> [byte] */
int64_t tk_csv_flush_w(int64_t writer) {
    TkCsvWriter *w = csvw_in(writer);
    if (!w) return tk_bytes_pack((const uint8_t *)"", 0);
    const char *res = csv_writer_flush(w);
    if (!res) return tk_bytes_pack((const uint8_t *)"", 0);
    int64_t h = tk_bytes_pack((const uint8_t *)res, tk_strlen(res));
    free((void *)res);   /* 127.422: csv_writer_flush's copy leaked on every flush */
    return h;
}

/* ── 135.2: hardened reader wrappers ─────────────────────────────────────
 *
 * ABI notes (docs/runtime-abi.md, and the same trap zip_glue.c records):
 *   §3  a `str` is a NUL-terminated char*, cast through i64.
 *   §4  an array handle points at data[0]; the length sits at handle[-1].
 *       A `@(byte)` stores ONE BYTE PER i64 SLOT, hence tk_bytes_unpack.
 *   §5  a struct is a flat i64 block, one slot per declared field, IN
 *       DECLARATION ORDER.
 *
 * §5 is the one that bites.  A slot order that disagrees with stdlib/csv.tki
 * still compiles and then reads whatever sits at the offset — 127.86 read the
 * first eight bytes of a string as an integer and reported 49.  The only
 * defence is that this file and csv.tki are edited together and that
 * test/conform/C008 reads EVERY declared field and checks the value.
 *
 *   $csvdialect            $csvopts
 *   0  delim      str      0  delim      str
 *   1  quote      str      1  quote      str
 *   2  hasheader  bool     2  encoding   str
 *   3  encoding   str      3  bom        str
 *   4  bom        bool     4  ragged     str
 *                          5  hasheader  bool
 */

#define TK_CSVDIALECT_SLOTS 5
#define TK_CSVOPTS_SLOTS    6

/* A one-character NUL-terminated toke str for a delimiter or quote, so that a
 * caller can PRINT what was detected.  A u8 would have to be turned back into
 * a character by every caller that wants to show it. */
static int64_t char_to_str(char c) {
    char *s = tks_dup(&c, c ? 1 : 0);   /* 127.295: sealed */
    return (int64_t)(intptr_t)(s ? s : "");
}

static int64_t dialect_to_block(const TkCsvDialect *d) {
    int64_t *b = (int64_t *)malloc(TK_CSVDIALECT_SLOTS * sizeof(int64_t));
    if (!b) return 0;
    b[0] = char_to_str(d->delim);
    b[1] = char_to_str(d->quote);
    b[2] = d->has_header ? 1 : 0;
    b[3] = (int64_t)(intptr_t)(d->encoding ? d->encoding : "");
    b[4] = d->bom ? 1 : 0;
    return (int64_t)(intptr_t)b;
}

/* csv.sniff(data:@(byte)) -> $csvdialect!$csverr
 *
 * On refusal the $err arm binds a csverr carrying the reason, the line and
 * the kind (127.102a). */
static const char tk_csv_sniff_fail[] =
    "csv.sniff: no dialect could be detected";
int64_t tk_csv_sniff_w(int64_t data) {
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    TkCsvDialect d;
    int ok = csv_sniff((const char *)bytes, bytes ? len : 0, &d);
    free(bytes);
    if (!ok) return csv_slot_core(0, tk_csv_sniff_fail, "delimiter");
    return csv_slot_core(dialect_to_block(&d), tk_csv_sniff_fail, "delimiter");
}

/* csv.opts(delim; quote; encoding; bom; ragged; hasheader) -> $csvopts
 *
 * Six explicit arguments rather than a defaults object the caller then
 * mutates: every policy this story defines is stated at the call site, so a
 * reader of the code can see which ragged-row rule is in force without
 * looking anywhere else.  "" selects the documented default for a field. */
int64_t tk_csv_opts_w(int64_t delim, int64_t quote, int64_t encoding, int64_t bom, int64_t ragged, int64_t hasheader) {
    int64_t *b = (int64_t *)malloc(TK_CSVOPTS_SLOTS * sizeof(int64_t));
    if (!b) return 0;
    b[0] = delim    ? delim    : (int64_t)(intptr_t)"";
    b[1] = quote    ? quote    : (int64_t)(intptr_t)"";
    b[2] = encoding ? encoding : (int64_t)(intptr_t)"";
    b[3] = bom      ? bom      : (int64_t)(intptr_t)"";
    b[4] = ragged   ? ragged   : (int64_t)(intptr_t)"";
    b[5] = hasheader ? 1 : 0;
    return (int64_t)(intptr_t)b;
}

/* csv.readeropts(data:@(byte); opts:$csvopts) -> $csvreader!$csverr */
static const char tk_csv_readeropts_fail[] =
    "csv.readeropts: the reader could not be opened with these options";
int64_t tk_csv_readeropts_w(int64_t data, int64_t opts) {
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    TkCsvOpts o;
    csv_opts_defaults(&o);
    if (opts) {
        const int64_t *p = (const int64_t *)(intptr_t)opts;
        o.delim      = (const char *)(intptr_t)p[0];
        o.quote      = (const char *)(intptr_t)p[1];
        o.encoding   = (const char *)(intptr_t)p[2];
        o.bom        = (const char *)(intptr_t)p[3];
        o.ragged     = (const char *)(intptr_t)p[4];
        o.has_header = p[5] ? 1 : 0;
    }
    /* csv_reader_open copies (and where needed transcodes) the buffer and
     * owns the copy, so the staging buffer is freed here. */
    TkCsvReader *r = csv_reader_open((const char *)bytes, bytes ? len : 0, &o);
    free(bytes);
    return csv_slot_core(csvr_out(r), tk_csv_readeropts_fail, "opts");
}

/* csv.dialect(r:$csvreader) -> $csvdialect — including a SNIFFED delimiter,
 * so that a delimiter the caller did not state is still visible to them. */
int64_t tk_csv_dialect_w(int64_t reader) {
    TkCsvDialect d = csv_reader_dialect(csvr_in(reader));
    return dialect_to_block(&d);
}

/* csv.atend(r:$csvreader) -> bool — end of data, as distinct from a refusal.
 * Both make csv.next return the `$err` arm; only this tells them apart before
 * the call, and the csverr's kind ("eof" or the refusal) after it. */
int64_t tk_csv_atend_w(int64_t reader) {
    return csv_reader_at_end(csvr_in(reader)) ? 1 : 0;
}

/* csv.raggedreport(r:$csvreader) -> @(str) — one line per ragged row seen so
 * far under the "report" (or "pad") policy.  A report a caller cannot read is
 * not a report, which is why this exists rather than a bare counter. */
int64_t tk_csv_raggedreport_w(int64_t reader) {
    const TkCsvReader *r = csvr_in(reader);
    uint64_t n = csv_reader_ragged_count(r);
    int64_t h = tk_arr_alloc((int64_t)n, (int64_t)n);
    if (!h) return tk_arr_alloc(0, 0);
    int64_t *blk = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < n; i++) {
        /* 127.295: a sealed copy; the reader keeps (and frees) its own. */
        blk[i] = (int64_t)(intptr_t)seal_copy(csv_reader_ragged_at(r, i));
    }
    return h;
}

/* csv.close(r:$csvreader) -> void.  138.63: a second close, or a close of
 * a value that is not a live reader, is a no-op (it was a double free). */
int64_t tk_csv_close_w(int64_t reader) {
    TkCsvReader *r = csvr_in(reader);
    if (!r) return 0;
    tk_handle_drop(r);
    csv_reader_free(r);
    return 0;
}
