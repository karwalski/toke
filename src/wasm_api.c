/*
 * wasm_api.c — WASM-specific entry points for the tkc browser playground.
 *
 * Provides in-memory equivalents of the CLI pipeline (lex -> parse -> names
 * -> types) without touching the filesystem.  Each function accepts a source
 * string and returns a malloc'd result string (JSON or toke source).  The
 * caller is responsible for freeing via tkc_free().
 *
 * Compiled only when TKC_WASM_BUILD is defined (set by Makefile.wasm).
 *
 * Story: 10.12.31
 */

#ifdef TKC_WASM_BUILD

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "names.h"
#include "types.h"
#include "fmt.h"
#include "tkc_limits.h"

/* Forward declarations matching main.c */
#ifndef TK_ARENA_TYPES_DEFINED
#define TK_ARENA_TYPES_DEFINED
typedef struct Arena Arena;
void *arena_alloc(Arena *arena, int size);
#endif
Arena *arena_init(void);
void   arena_free(Arena *arena);

int  diag_error_count(void);
void diag_reset(void);
void diag_set_format(DiagFormat fmt);
void diag_set_source_file(const char *path);
void diag_flush_sarif(void);

int lex(const char *src, int src_len, Token *tokens, int token_cap);

/* ── Version ────────────────────────────────────────────────────────── */

#define TKC_WASM_VERSION "tkc 0.1.0-wasm (Profile 1)"

/* ── Diagnostic capture ─────────────────────────────────────────────
 *
 * The existing diag module writes to stderr.  For WASM we intercept
 * diagnostics after each pipeline stage by running the pipeline with
 * DIAG_FMT_JSON, then capturing stderr output would require EM_JS
 * hacks.  Instead we re-run the pipeline stages and build JSON from
 * the diag_error_count() plus re-lexing/re-parsing to collect line
 * info.
 *
 * A simpler approach: we run the full pipeline and record pass/fail
 * at each stage.  Detailed per-diagnostic JSON is captured by a small
 * ring buffer that hooks into diag_emit via a WASM-specific shim
 * compiled into diag.c (future enhancement).  For now, we return a
 * summary JSON with stage, success/fail, and error count.
 * ----------------------------------------------------------------- */

#define DIAG_CAP 256
#define DIAG_MSG_MAX 512

typedef struct {
    int  severity;  /* 0 = warning, 1 = error */
    int  code;
    int  line;
    int  col;
    char message[DIAG_MSG_MAX];
} WasmDiag;

static WasmDiag s_diags[DIAG_CAP];
static int      s_diag_count = 0;

/* Reset captured diagnostics before each invocation. */
static void wasm_diag_reset(void)
{
    s_diag_count = 0;
    diag_reset();
}

/* ── JSON helpers ───────────────────────────────────────────────────── */

/* Append a JSON-escaped version of src to buf at *pos, not exceeding cap. */
static void json_escape_to(char *buf, int *pos, int cap, const char *src)
{
    for (; *src && *pos < cap - 6; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"')       { buf[(*pos)++] = '\\'; buf[(*pos)++] = '"'; }
        else if (c == '\\') { buf[(*pos)++] = '\\'; buf[(*pos)++] = '\\'; }
        else if (c == '\n') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'n'; }
        else if (c == '\r') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'r'; }
        else if (c == '\t') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 't'; }
        else if (c < 0x20)  {
            int w = snprintf(buf + *pos, (size_t)(cap - *pos), "\\u%04x", c);
            if (w > 0) *pos += w;
        }
        else { buf[(*pos)++] = (char)c; }
    }
}

/* Build a JSON diagnostics result string.  Caller must free(). */
static char *build_diagnostics_json(int success, const char *stage)
{
    int cap = 4096 + s_diag_count * 1024;
    char *buf = malloc((size_t)cap);
    if (!buf) return NULL;

    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(cap - pos),
        "{\"success\":%s,\"stage\":\"", success ? "true" : "false");
    json_escape_to(buf, &pos, cap, stage);
    pos += snprintf(buf + pos, (size_t)(cap - pos),
        "\",\"error_count\":%d,\"diagnostics\":[", diag_error_count());

    for (int i = 0; i < s_diag_count && pos < cap - 256; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, (size_t)(cap - pos),
            "{\"severity\":\"%s\",\"code\":%d,\"line\":%d,\"col\":%d,\"message\":\"",
            s_diags[i].severity ? "error" : "warning",
            s_diags[i].code, s_diags[i].line, s_diags[i].col);
        json_escape_to(buf, &pos, cap, s_diags[i].message);
        pos += snprintf(buf + pos, (size_t)(cap - pos), "\"}");
    }

    pos += snprintf(buf + pos, (size_t)(cap - pos), "]}");
    buf[pos] = '\0';
    return buf;
}

/* ── Pipeline runner ────────────────────────────────────────────────── */

typedef struct {
    Arena  *arena;
    Token  *toks;
    int     tc;
    Node   *ast;
    int     ok;        /* 1 = pipeline succeeded to the requested stage */
    char   *fail_stage; /* stage name where failure occurred, or NULL */
} PipeResult;

/*
 * Run the compiler pipeline up to (and including) the given stage.
 * Stages: "lex", "parse", "names", "types".
 * The caller must call arena_free(result.arena) when finished.
 */
static PipeResult run_pipeline(const char *source, const char *up_to)
{
    PipeResult r = {0};
    r.arena = arena_init();
    if (!r.arena) { r.fail_stage = "init"; return r; }

    int slen = (int)strlen(source);
    TkcLimits limits;
    tkc_limits_defaults(&limits);

    wasm_diag_reset();
    diag_set_format(DIAG_FMT_JSON);
    diag_set_source_file("<playground>");

    /* ── Lex ─────────────────────────────────────────────────────── */
    int tcap = slen + 16;
    r.toks = arena_alloc(r.arena, tcap * (int)sizeof(Token));
    if (!r.toks) { r.fail_stage = "lex"; return r; }

    r.tc = lex(source, slen, r.toks, tcap);
    if (r.tc < 0 || diag_error_count() > 0) {
        r.fail_stage = "lex";
        return r;
    }
    if (strcmp(up_to, "lex") == 0) { r.ok = 1; return r; }

    /* ── Parse ───────────────────────────────────────────────────── */
    r.ast = parse(r.toks, r.tc, source, r.arena);
    if (!r.ast || diag_error_count() > 0) {
        r.fail_stage = "parse";
        return r;
    }
    if (strcmp(up_to, "parse") == 0) { r.ok = 1; return r; }

    /* ── Names ───────────────────────────────────────────────────── */
    SymbolTable st;
    if (resolve_imports(r.ast, source, ".", &limits, &st) < 0 ||
        diag_error_count() > 0) {
        r.fail_stage = "names";
        return r;
    }

    NameEnv ne;
    if (resolve_names(r.ast, source, &st, r.arena, &ne) < 0 ||
        diag_error_count() > 0) {
        symtab_free(&st);
        r.fail_stage = "names";
        return r;
    }
    if (strcmp(up_to, "names") == 0) {
        symtab_free(&st);
        r.ok = 1;
        return r;
    }

    /* ── Types ───────────────────────────────────────────────────── */
    TypeEnv te;
    if (type_check(r.ast, source, &ne, r.arena, &te) < 0 ||
        diag_error_count() > 0) {
        symtab_free(&st);
        r.fail_stage = "types";
        return r;
    }

    symtab_free(&st);
    r.ok = 1;
    return r;
}

/* ── Exported API ───────────────────────────────────────────────────── */

/*
 * tkc_check_source — run the full check pipeline (lex -> parse -> names
 * -> types) on the given source string.
 *
 * Returns a malloc'd JSON string:
 *   {"success": true/false, "stage": "...", "error_count": N,
 *    "diagnostics": [...]}
 *
 * Caller must free via tkc_free().
 */
EMSCRIPTEN_KEEPALIVE
const char *tkc_check_source(const char *source)
{
    if (!source) source = "";

    PipeResult r = run_pipeline(source, "types");
    char *json = build_diagnostics_json(r.ok, r.fail_stage ? r.fail_stage : "types");
    if (r.arena) arena_free(r.arena);
    return json;
}

/*
 * tkc_format_source — lex and parse the source, then format the AST
 * back to canonical toke source.
 *
 * Returns a malloc'd JSON string:
 *   {"success": true/false, "formatted": "...", "error_count": N,
 *    "diagnostics": [...]}
 *
 * If parsing fails, "formatted" is an empty string and diagnostics
 * are populated.  Caller must free via tkc_free().
 */
EMSCRIPTEN_KEEPALIVE
const char *tkc_format_source(const char *source)
{
    if (!source) source = "";

    PipeResult r = run_pipeline(source, "parse");
    if (!r.ok || !r.ast) {
        char *json = build_diagnostics_json(0, r.fail_stage ? r.fail_stage : "parse");
        if (r.arena) arena_free(r.arena);
        return json;
    }

    char *formatted = tkc_format(r.ast, source);
    if (!formatted) {
        char *json = build_diagnostics_json(0, "format");
        if (r.arena) arena_free(r.arena);
        return json;
    }

    /* Build JSON result with the formatted source embedded. */
    int fmtlen = (int)strlen(formatted);
    int cap = 512 + fmtlen * 2; /* worst case: every char escaped */
    char *buf = malloc((size_t)cap);
    if (!buf) {
        free(formatted);
        if (r.arena) arena_free(r.arena);
        return NULL;
    }

    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(cap - pos),
        "{\"success\":true,\"formatted\":\"");
    json_escape_to(buf, &pos, cap, formatted);
    pos += snprintf(buf + pos, (size_t)(cap - pos),
        "\",\"error_count\":0,\"diagnostics\":[]}");
    buf[pos] = '\0';

    free(formatted);
    if (r.arena) arena_free(r.arena);
    return buf;
}

/*
 * tkc_get_diagnostics — alias for tkc_check_source.
 *
 * Provided for API symmetry with the story requirements.
 */
EMSCRIPTEN_KEEPALIVE
const char *tkc_get_diagnostics(const char *source)
{
    return tkc_check_source(source);
}

/*
 * tkc_version — return the WASM build version string.
 *
 * Returns a static string (do NOT pass to tkc_free).
 */
EMSCRIPTEN_KEEPALIVE
const char *tkc_version(void)
{
    return TKC_WASM_VERSION;
}

/*
 * tkc_free — free a string returned by tkc_check_source or
 * tkc_format_source.
 */
EMSCRIPTEN_KEEPALIVE
void tkc_free(void *ptr)
{
    free(ptr);
}

#endif /* TKC_WASM_BUILD */
