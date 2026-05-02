/*
 * main.c — CLI entry point and pipeline driver for the toke reference
 *           compiler (tkc).
 *
 * =========================================================================
 * Role
 * =========================================================================
 * This file owns:
 *   1. CLI argument parsing (flags, limit overrides, config file loading).
 *   2. Pipeline orchestration — the fixed sequence of compiler phases:
 *        read source -> lex -> parse -> (fmt?) -> resolve imports ->
 *        resolve names -> type check -> (emit interface?) ->
 *        emit LLVM IR -> (emit asm / compile binary).
 *   3. Exit-code semantics (EUSAGE / ECOMPILE / EINTERNAL).
 *   4. Progress-bar lifecycle (init / update / done).
 *
 * Fast-path modes (--fmt, --pretty, --expand, --check) exit early
 * before code generation.
 *
 * Story: 1.2.9  Branch: feature/compiler-cli
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parser.h"  /* Token, Node, parse(); Arena/diag_emit forwards */
#include "names.h"   /* resolve_imports(), resolve_names(), SymbolTable, NameEnv */
#include "types.h"   /* type_check(), TypeEnv */
#include "ir.h"      /* emit_interface() */
#include "llvm.h"    /* emit_llvm_ir(), compile_binary(), CodegenEnv */
#include "fmt.h"     /* tkc_format() */
#include "ast_json.h" /* ast_dump_json() */
#include "sourcemap.h" /* SourceMap, sourcemap_*() */
#include "companion.h" /* emit_companion() */
#include "compress.h"  /* compress_text(), decompress_text(), compress_stream_*() */
#include "tkc_limits.h"
#include "config.h"
#include "progress.h"
#include "migrate.h"
#include "lint.h"
#include "tkir.h"

/* Forward declarations for stubs not yet exported by their headers */
#ifndef TK_ARENA_TYPES_DEFINED
#define TK_ARENA_TYPES_DEFINED
typedef struct Arena Arena;
void *arena_alloc(Arena *arena, int size);
#endif
Arena *arena_init(void);
void   arena_free(Arena *arena);

int  diag_error_count(void);
void diag_reset(void);

int lex(const char *src, int src_len, Token *tokens, int token_cap, Profile profile);

/* ── Constants ─────────────────────────────────────────────────────── */

#define VERSION   "toke 0.1.0"
#define EUSAGE    TKC_EXIT_USAGE
#define EINTERNAL TKC_EXIT_INTERNAL
#define ECOMPILE  TKC_EXIT_COMPILE

static const char HELP[] =
    "Usage: toke [flags] <source-file>\n"
    "\n"
    "Flags:\n"
    "  --target <triple>   LLVM target triple (default: native)\n"
    "  --out <path>        Output binary path (default: source stem)\n"
    "  --emit-interface    Also emit .tki interface file\n"
    "  --emit-llvm         Emit LLVM IR (.ll) instead of compiling to binary\n"
    "  --emit-asm          Emit assembly (.s) instead of compiling to binary\n"
    "  -g / --debug        Emit DWARF debug metadata for lldb/gdb\n"
    "  -O0/-O1/-O2/-O3    Optimization level (default: -O1)\n"
    "  --fmt               Format source to canonical form and print to stdout\n"
    "  --pretty            Pretty-print with whitespace for human readability\n"
    "  --expand            Expand abbreviated identifiers as inline comments\n"
    "  --sourcemap         Emit .map source map alongside --fmt/--pretty output\n"
    "  --dump-ast          Dump parsed AST as JSON to stdout and exit\n"
    "  --check             Type check only; no code generation\n"
    "  --companion         Generate .tkc.md companion file skeleton to stdout\n"
    "  --companion-out <p> Write companion file to <p> instead of stdout\n"
    "  --verify-companion <f> Verify companion file hash against source\n"
    "  --companion-diff <f>   Compare source against companion, report divergences\n"
    "  --compress          Compress text from stdin to reduced-token form (stdout)\n"
    "  --decompress        Decompress text from stdin back to original form (stdout)\n"
    "  --compress-stream   Stream-compress stdin line-by-line, emitting chunks\n"
    "  --lint              Run lint rules and report diagnostics\n"
    "  --fix               With --lint, auto-fix mechanical violations in-place\n"
    "  --migrate           Migrate legacy .tk file to default syntax (stdout)\n"
    "  --legacy            Legacy mode (80-char syntax, uppercase keywords)\n"
    "  --profile1          Deprecated alias for --legacy\n"
    "  --profile2          Deprecated alias for default mode\n"
    "  --diag-json         Emit diagnostics as JSON (default when not a tty)\n"
    "  --diag-text         Emit diagnostics as human-readable text\n"
    "  --diag-sarif        Emit diagnostics as SARIF v2.1.0 JSON to stdout\n"
    "\n"
    "Limit overrides (per-session):\n"
    "  --max-funcs=N       Max function signatures (default: 128)\n"
    "  --max-locals=N      Max locals per function (default: 128)\n"
    "  --max-params=N      Max params per function / fields per struct (default: 16)\n"
    "  --max-structs=N     Max struct type definitions (default: 64)\n"
    "  --max-imports=N     Max import alias mappings (default: 32)\n"
    "  --max-in-flight=N   Max concurrent imports in flight (default: 64)\n"
    "  --max-avail=N       Max available modules in directory (default: 256)\n"
    "  --arena-block=N     Arena block size in bytes (default: 65536)\n"
    "  --max-iters=N       Max loop iterations at runtime (default: 0 = unlimited)\n"
    "  --show-limits       Print effective limits and exit\n"
    "\n"
    "Configuration:\n"
    "  --config=PATH       Load config from PATH (default: ./tkc.toml)\n"
    "\n"
    "  --version           Print version and exit\n"
    "  --help              Print this help and exit";

/* ── Utility ───────────────────────────────────────────────────────── */

/*
 * build_sourcemap — Walk compact and expanded output in parallel,
 * identifying non-whitespace runs (tokens) and recording their positions.
 *
 * Both strings are produced from the same AST, so tokens appear in the
 * same order.  We track line/col through each and pair them up.
 */
static void build_sourcemap(SourceMap *sm, const char *compact, const char *expanded)
{
    int ci = 0, ei = 0;
    int cline = 1, ccol = 0;
    int eline = 1, ecol = 0;

    while (compact[ci] && expanded[ei]) {
        /* Skip whitespace in compact output, tracking line/col */
        while (compact[ci] && (compact[ci] == ' ' || compact[ci] == '\n' ||
               compact[ci] == '\r' || compact[ci] == '\t')) {
            if (compact[ci] == '\n') { cline++; ccol = 0; }
            else { ccol++; }
            ci++;
        }
        /* Skip whitespace in expanded output, tracking line/col */
        while (expanded[ei] && (expanded[ei] == ' ' || expanded[ei] == '\n' ||
               expanded[ei] == '\r' || expanded[ei] == '\t')) {
            if (expanded[ei] == '\n') { eline++; ecol = 0; }
            else { ecol++; }
            ei++;
        }
        if (!compact[ci] || !expanded[ei]) break;

        /* Record start of this token */
        int tok_cline = cline, tok_ccol = ccol;
        int tok_eline = eline, tok_ecol = ecol;
        int len = 0;

        /* Advance through matching non-whitespace characters */
        while (compact[ci] && expanded[ei] &&
               compact[ci] != ' '  && compact[ci] != '\n' &&
               compact[ci] != '\r' && compact[ci] != '\t' &&
               expanded[ei] != ' ' && expanded[ei] != '\n' &&
               expanded[ei] != '\r' && expanded[ei] != '\t') {
            ci++; ccol++;
            ei++; ecol++;
            len++;
        }

        if (len > 0) {
            sourcemap_add(sm, tok_cline, tok_ccol,
                          tok_eline, tok_ecol, len);
        }
    }
}

/*
 * stem — Extract the filename stem (no directory, no extension) from path.
 *
 * Given "/foo/bar.tk" writes "bar" into buf.  Used to derive the default
 * output binary name when --out is not specified.
 */
static void stem(const char *path, char *buf, int n)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    strncpy(buf, b, (size_t)(n - 1));
    buf[n - 1] = '\0';
    char *d = strrchr(buf, '.');
    if (d) *d = '\0';
}

/* ── main ──────────────────────────────────────────────────────────── */

/*
 * main — Parse CLI flags, load config, then drive the compiler pipeline.
 *
 * Phase order:
 *   1. Pre-scan argv for --config=PATH, load tkc.toml defaults.
 *   2. Full flag parse — populate mode booleans, limits, paths.
 *   3. Read source file into a malloc'd buffer.
 *   4. Lex -> Parse -> (optional: fmt / pretty / expand early exit).
 *   5. Resolve imports -> Resolve names -> Type check.
 *   6. (optional: --check early exit).
 *   7. Derive output path, optionally emit .tki interface.
 *   8. Emit LLVM IR -> (--emit-llvm / --emit-asm / compile binary).
 *   9. Cleanup: flush SARIF, free arena & source buffer.
 *
 * Returns 0 on success, EUSAGE (64) for bad arguments, ECOMPILE (65)
 * for source errors, or EINTERNAL (70) for tool failures.
 */
int main(int argc, char **argv)
{
    const char *tgt = NULL, *out = NULL, *src = NULL;
    const char *config_path = NULL;
    int emit_iface = 0, check_only = 0, djson = 0, dtext = 0, dsarif = 0;
    int emit_ll = 0, emit_asm = 0, opt_level = 1, fmt_only = 0, debug_info = 0;
    int pretty = 0, expand = 0, sourcemap = 0;
    int dump_ast = 0;
    int migrate = 0;
    int do_lint = 0, do_fix = 0;
    int emit_tkir_flag = 0;
    int do_compress = 0, do_decompress = 0, do_compress_stream = 0;
    int companion = 0;
    const char *companion_out = NULL;
    const char *verify_companion_path = NULL;
    const char *companion_diff_comp = NULL;
    int show_limits = 0;
    Profile profile = PROFILE_DEFAULT;
    TkcLimits limits;
    tkc_limits_defaults(&limits);

    /* Pre-scan for --config=PATH before full flag parsing */
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--config=", 9)) {
            config_path = argv[i] + 9;
        }
    }

    /* Load config: CLI-specified path, or default tkc.toml */
    {
        const char *cfg = config_path ? config_path : "tkc.toml";
        int cr = tkc_load_config(cfg, &limits);
        if (cr == -2) {
            fprintf(stderr, "tkc: failed to parse config '%s'\n", cfg);
            return EUSAGE;
        }
        if (cr == -1 && config_path) {
            /* Explicit --config path not found is an error */
            fprintf(stderr, "tkc: config file not found: '%s'\n", config_path);
            return EUSAGE;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version"))        { puts(VERSION); return 0; }
        else if (!strcmp(argv[i], "--help"))       { puts(HELP);   return 0; }
        else if (!strcmp(argv[i], "--emit-interface")) emit_iface = 1;
        else if (!strcmp(argv[i], "--emit-llvm"))      emit_ll = 1;
        else if (!strcmp(argv[i], "--emit-asm"))       emit_asm = 1;
        else if (!strcmp(argv[i], "--fmt"))             fmt_only = 1;
        else if (!strcmp(argv[i], "--pretty"))          pretty = 1;
        else if (!strcmp(argv[i], "--expand"))          expand = 1;
        else if (!strcmp(argv[i], "--sourcemap"))      sourcemap = 1;
        else if (!strcmp(argv[i], "--check"))          check_only = 1;
        else if (!strcmp(argv[i], "--dump-ast"))       dump_ast = 1;
        else if (!strcmp(argv[i], "--lint"))             do_lint = 1;
        else if (!strcmp(argv[i], "--fix"))              do_fix = 1;
        else if (!strcmp(argv[i], "--emit-tkir"))       emit_tkir_flag = 1;
        else if (!strcmp(argv[i], "--migrate"))         migrate = 1;
        else if (!strcmp(argv[i], "--compress"))        do_compress = 1;
        else if (!strcmp(argv[i], "--decompress"))      do_decompress = 1;
        else if (!strcmp(argv[i], "--compress-stream")) do_compress_stream = 1;
        else if (!strcmp(argv[i], "--companion"))    companion = 1;
        else if (!strcmp(argv[i], "--companion-out")) {
            companion = 1;
            if (++i >= argc) { fputs("tkc: --companion-out requires an argument\n", stderr); return EUSAGE; }
            companion_out = argv[i];
        }
        else if (!strcmp(argv[i], "--verify-companion")) {
            if (++i >= argc) { fputs("tkc: --verify-companion requires an argument\n", stderr); return EUSAGE; }
            verify_companion_path = argv[i];
        }
        else if (!strcmp(argv[i], "--companion-diff")) {
            if (++i >= argc) { fputs("tkc: --companion-diff requires a companion file argument\n", stderr); return EUSAGE; }
            companion_diff_comp = argv[i];
        }
        else if (!strcmp(argv[i], "--legacy"))       profile = PROFILE_LEGACY;
        /* deprecated aliases */
        else if (!strcmp(argv[i], "--profile1"))   profile = PROFILE_LEGACY;
        else if (!strcmp(argv[i], "--profile2"))   { /* default mode, no-op */ }
        else if (!strcmp(argv[i], "--phase1"))     profile = PROFILE_LEGACY;
        else if (!strcmp(argv[i], "--phase2"))     { /* default mode, no-op */ }
        else if (!strcmp(argv[i], "-g"))                debug_info = 1;
        else if (!strcmp(argv[i], "--debug"))          debug_info = 1;
        else if (!strcmp(argv[i], "-O0"))             opt_level = 0;
        else if (!strcmp(argv[i], "-O1"))             opt_level = 1;
        else if (!strcmp(argv[i], "-O2"))             opt_level = 2;
        else if (!strcmp(argv[i], "-O3"))             opt_level = 3;
        else if (!strcmp(argv[i], "--diag-json"))  djson = 1;
        else if (!strcmp(argv[i], "--diag-text"))  dtext = 1;
        else if (!strcmp(argv[i], "--diag-sarif")) dsarif = 1;
        else if (!strcmp(argv[i], "--show-limits")) show_limits = 1;
        else if (!strncmp(argv[i], "--max-funcs=", 12))   limits.max_funcs = atoi(argv[i] + 12);
        else if (!strncmp(argv[i], "--max-locals=", 13))  limits.max_locals = atoi(argv[i] + 13);
        else if (!strncmp(argv[i], "--max-params=", 13))  limits.max_params = atoi(argv[i] + 13);
        else if (!strncmp(argv[i], "--max-structs=", 14)) limits.max_struct_types = atoi(argv[i] + 14);
        else if (!strncmp(argv[i], "--max-imports=", 14)) limits.max_imports = atoi(argv[i] + 14);
        else if (!strncmp(argv[i], "--max-in-flight=", 16)) limits.max_imports_in_flight = atoi(argv[i] + 16);
        else if (!strncmp(argv[i], "--max-avail=", 12)) limits.max_avail_modules = atoi(argv[i] + 12);
        else if (!strncmp(argv[i], "--arena-block=", 14)) limits.arena_block_size = atoi(argv[i] + 14);
        else if (!strncmp(argv[i], "--max-iters=", 12))  limits.max_iters = atoi(argv[i] + 12);
        else if (!strncmp(argv[i], "--config=", 9)) { /* already handled */ }
        else if (!strcmp(argv[i], "--target")) {
            if (++i >= argc) { fputs("tkc: --target requires an argument\n", stderr); return EUSAGE; }
            tgt = argv[i];
        } else if (!strcmp(argv[i], "--out")) {
            if (++i >= argc) { fputs("tkc: --out requires an argument\n", stderr); return EUSAGE; }
            out = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tkc: unknown flag: %s\n", argv[i]);
            return EUSAGE;
        } else {
            if (src) { fputs("tkc: only one source file supported\n", stderr); return EUSAGE; }
            src = argv[i];
        }
    }

    if (show_limits) {
        printf("tkc effective limits:\n");
        printf("  max-funcs    = %d\n", limits.max_funcs);
        printf("  max-locals   = %d\n", limits.max_locals);
        printf("  max-params   = %d\n", limits.max_params);
        printf("  max-structs  = %d\n", limits.max_struct_types);
        printf("  max-imports  = %d\n", limits.max_imports);
        printf("  max-in-flight= %d\n", limits.max_imports_in_flight);
        printf("  max-avail    = %d\n", limits.max_avail_modules);
        printf("  arena-block  = %d\n", limits.arena_block_size);
        printf("  max-iters    = %d%s\n", limits.max_iters, limits.max_iters ? "" : " (unlimited)");
        return 0;
    }
    /* --compress / --decompress / --compress-stream: standalone stdin modes */
    if ((do_compress + do_decompress + do_compress_stream) > 1) {
        fputs("tkc: --compress, --decompress, and --compress-stream are mutually exclusive\n", stderr);
        return EUSAGE;
    }
    if (do_compress || do_decompress || do_compress_stream) {
        /* Read all of stdin into a malloc'd buffer */
        char *ibuf = NULL;
        size_t ilen = 0, icap = 0;
        int ch;
        while ((ch = fgetc(stdin)) != EOF) {
            if (ilen >= icap) {
                size_t newcap = icap == 0 ? 4096 : icap * 2;
                char *tmp = realloc(ibuf, newcap);
                if (!tmp) { free(ibuf); fputs("tkc: out of memory\n", stderr); return EINTERNAL; }
                ibuf = tmp; icap = newcap;
            }
            ibuf[ilen++] = (char)ch;
        }

        if (do_compress_stream) {
            /* Stream-compress: feed line-by-line to streaming API */
            CompressStreamCtx sctx;
            compress_stream_init(&sctx, stdout);
            size_t si = 0;
            while (si < ilen) {
                size_t line_start = si;
                while (si < ilen && ibuf[si] != '\n') si++;
                if (si < ilen) si++; /* include newline */
                compress_stream_feed(&sctx, ibuf + line_start,
                                     (int)(si - line_start));
            }
            compress_stream_flush(&sctx);
            free(ibuf);
            return 0;
        }

        /* Batch compress or decompress */
        size_t out_size = ilen * 4 + 128;
        char *obuf = malloc(out_size);
        if (!obuf) { free(ibuf); fputs("tkc: out of memory\n", stderr); return EINTERNAL; }

        int written;
        if (do_compress) {
            /* Auto-detect input type for schema-aware compression */
            CompressInputType ctype = detect_input_type(ibuf ? ibuf : "", ilen);
            if (ctype == COMPRESS_JSON)
                written = compress_json(ibuf ? ibuf : "", ilen, obuf);
            else if (ctype == COMPRESS_CSV)
                written = compress_csv(ibuf ? ibuf : "", ilen, obuf);
            else
                written = compress_text(ibuf ? ibuf : "", ilen, obuf);
        } else {
            written = decompress_text(ibuf ? ibuf : "", ilen, obuf);
        }

        free(ibuf);
        if (written < 0) {
            free(obuf);
            fputs("tkc: compression error\n", stderr);
            return EINTERNAL;
        }
        fwrite(obuf, 1, (size_t)written, stdout);
        free(obuf);
        return 0;
    }

    /* --verify-companion: standalone mode, no source file needed */
    if (verify_companion_path) return verify_companion(verify_companion_path);
    if (!src)            { puts(HELP); return EUSAGE; }
    if ((djson + dtext + dsarif) > 1) { fputs("tkc: --diag-json, --diag-text, and --diag-sarif are mutually exclusive\n", stderr); return EUSAGE; }

    diag_set_format(dsarif ? DIAG_FMT_SARIF :
                    djson  ? DIAG_FMT_JSON  :
                    dtext  ? DIAG_FMT_TEXT  :
                    isatty(STDOUT_FILENO) ? DIAG_FMT_TEXT : DIAG_FMT_JSON);
    diag_set_source_file(src);

    /* --migrate: try default profile first (handles partially migrated files),
     * fall back to legacy if too many lex errors. Actual migration is
     * idempotent — already-migrated constructs pass through unchanged. */

    /* Progress bar: skip for fast-path modes */
    int fast_path = fmt_only || pretty || expand || check_only || dump_ast || migrate || companion || companion_diff_comp || do_compress || do_decompress || do_compress_stream || do_lint;
    progress_init(fast_path);

    /* Read source file — only malloc in the pipeline */
    FILE *f = fopen(src, "rb");
    if (!f) { fprintf(stderr, "tkc: cannot open '%s'\n", src); return EUSAGE; }
    fseek(f, 0, SEEK_END);
    long slen = ftell(f);
    rewind(f);
    char *sbuf = malloc((size_t)slen + 1);
    if (!sbuf || (long)fread(sbuf, 1, (size_t)slen, f) != slen) {
        fclose(f); free(sbuf);
        fprintf(stderr, "tkc: failed to read '%s'\n", src);
        return sbuf ? EUSAGE : EINTERNAL;
    }
    sbuf[slen] = '\0';
    fclose(f);

    Arena *arena = arena_init();
    if (!arena) { free(sbuf); fputs("tkc: arena_init failed\n", stderr); return EINTERNAL; }

    int rc = 0;

    /* Lex */
    int tcap = (int)(slen + 16);
    Token *toks = arena_alloc(arena, tcap * (int)sizeof(Token));
    if (!toks) { rc = EINTERNAL; goto done; }
    int tc;
    if (migrate) {
        /* --migrate does its own prepass + re-lex internally.
         * Pass raw source directly — no external lex needed. */
        if (tkc_migrate(sbuf, (int)slen, NULL, 0, stdout) < 0) {
            rc = EINTERNAL;
        }
        goto done;
    }
    tc = lex(sbuf, (int)slen, toks, tcap, profile);
    if (tc < 0 || diag_error_count() > 0) { rc = ECOMPILE; goto done; }
    progress_update(10);

    /* Parse */
    Node *ast = parse(toks, tc, sbuf, arena, profile);
    if (!ast || diag_error_count() > 0) { rc = ECOMPILE; goto done; }
    progress_update(30);

    /* --companion-diff: compare source AST against existing companion */
    if (companion_diff_comp) {
        rc = companion_diff(src, sbuf, slen, ast, companion_diff_comp);
        goto done;
    }

    /* --dump-ast: serialise AST as JSON to stdout, then exit */
    if (dump_ast) {
        ast_dump_json(ast, sbuf, stdout);
        goto done;
    }

    /* --lint (and optionally --fix): run lint rules, report/fix, then exit */
    if (do_lint) {
        LintResult lr;
        if (tkc_lint(ast, sbuf, (int)slen, NULL, &lr) < 0) {
            rc = EINTERNAL; goto done;
        }
        if (lr.count == 0) {
            fprintf(stderr, "tkc: no lint warnings\n");
            lint_result_free(&lr);
            goto done;
        }

        /* Print all diagnostics */
        for (int li = 0; li < lr.count; li++) {
            LintDiag *d = &lr.items[li];
            fprintf(stderr, "%s:%d:%d: warning: [%s] %s",
                    src, d->line, d->col, d->rule_id, d->message);
            if (do_fix && d->fixable)
                fprintf(stderr, " [auto-fixed]");
            fprintf(stderr, "\n");
        }

        /* --fix: apply fixable removals in reverse offset order */
        if (do_fix) {
            /* Collect fixable indices */
            int fix_count = 0;
            for (int li = 0; li < lr.count; li++) {
                if (lr.items[li].fixable) fix_count++;
            }
            if (fix_count > 0) {
                /* Sort fixable diags by span_start descending (simple selection sort) */
                int *fix_idx = malloc((size_t)fix_count * sizeof(int));
                if (!fix_idx) { lint_result_free(&lr); rc = EINTERNAL; goto done; }
                int fi = 0;
                for (int li = 0; li < lr.count; li++) {
                    if (lr.items[li].fixable) fix_idx[fi++] = li;
                }
                /* Sort descending by span_start */
                for (int a = 0; a < fix_count - 1; a++) {
                    for (int b = a + 1; b < fix_count; b++) {
                        if (lr.items[fix_idx[a]].span_start < lr.items[fix_idx[b]].span_start) {
                            int tmp = fix_idx[a]; fix_idx[a] = fix_idx[b]; fix_idx[b] = tmp;
                        }
                    }
                }

                /* Apply removals in reverse order to preserve earlier offsets */
                char *buf = malloc((size_t)slen + 1);
                if (!buf) { free(fix_idx); lint_result_free(&lr); rc = EINTERNAL; goto done; }
                memcpy(buf, sbuf, (size_t)slen + 1);
                int cur_len = (int)slen;

                for (int k = 0; k < fix_count; k++) {
                    LintDiag *d = &lr.items[fix_idx[k]];
                    int rm_start = d->span_start;
                    int rm_end   = d->span_end;
                    if (rm_start < 0 || rm_end > cur_len || rm_start >= rm_end) continue;
                    int rm_len = rm_end - rm_start;
                    memmove(buf + rm_start, buf + rm_end, (size_t)(cur_len - rm_end));
                    cur_len -= rm_len;
                    buf[cur_len] = '\0';
                }

                /* Write modified source back to the file */
                FILE *wf = fopen(src, "wb");
                if (!wf) {
                    fprintf(stderr, "tkc: cannot write '%s'\n", src);
                    free(buf); free(fix_idx); lint_result_free(&lr);
                    rc = EUSAGE; goto done;
                }
                fwrite(buf, 1, (size_t)cur_len, wf);
                fclose(wf);
                fprintf(stderr, "tkc: fixed %d violation(s) in %s\n", fix_count, src);
                free(buf);
                free(fix_idx);
            }
        }
        lint_result_free(&lr);
        goto done;
    }

    /* --fmt: format and print to stdout, then exit */
    if (fmt_only) {
        char *formatted = tkc_format(ast, sbuf);
        if (!formatted) { rc = EINTERNAL; goto done; }
        fputs(formatted, stdout);
        if (sourcemap) {
            FmtOptions sm_opts = { 1, 0 };
            char *expanded = tkc_format_pretty(ast, sbuf, sm_opts);
            if (expanded) {
                SourceMap sm;
                sourcemap_init(&sm);
                build_sourcemap(&sm, formatted, expanded);
                char map_path[PATH_BUF];
                snprintf(map_path, sizeof(map_path), "%s.map", src);
                sourcemap_emit_json(&sm, map_path);
                sourcemap_free(&sm);
                free(expanded);
            }
        }
        free(formatted);
        goto done;
    }

    /* --pretty / --expand: pretty-print and/or expand, then exit */
    if (pretty || expand) {
        FmtOptions opts = { pretty, expand };
        char *formatted = tkc_format_pretty(ast, sbuf, opts);
        if (!formatted) { rc = EINTERNAL; goto done; }
        fputs(formatted, stdout);
        if (sourcemap) {
            char *compact = tkc_format(ast, sbuf);
            if (compact) {
                SourceMap sm;
                sourcemap_init(&sm);
                build_sourcemap(&sm, compact, formatted);
                char map_path[PATH_BUF];
                snprintf(map_path, sizeof(map_path), "%s.map", src);
                sourcemap_emit_json(&sm, map_path);
                sourcemap_free(&sm);
                free(compact);
            }
        }
        free(formatted);
        goto done;
    }

    /* Resolve imports */
    SymbolTable st;
    if (resolve_imports(ast, sbuf, ".", &limits, &st) < 0 || diag_error_count() > 0) { rc = ECOMPILE; goto done; }

    /* Resolve names */
    NameEnv ne;
    if (resolve_names(ast, sbuf, &st, arena, &ne) < 0 || diag_error_count() > 0) {
        symtab_free(&st); rc = ECOMPILE; goto done;
    }
    progress_update(50);

    /* Type check */
    TypeEnv te;
    if (type_check(ast, sbuf, &ne, arena, &te) < 0 || diag_error_count() > 0) {
        symtab_free(&st); rc = ECOMPILE; goto done;
    }

    progress_update(70);

    if (check_only) { symtab_free(&st); goto done; }

    /* --companion: generate .tkc.md skeleton and exit */
    if (companion) {
        FILE *comp_out = stdout;
        if (companion_out) {
            comp_out = fopen(companion_out, "w");
            if (!comp_out) {
                fprintf(stderr, "tkc: cannot open '%s' for writing\n", companion_out);
                symtab_free(&st); rc = EUSAGE; goto done;
            }
        }
        emit_companion(comp_out, src, sbuf, slen, ast);
        if (companion_out) fclose(comp_out);
        symtab_free(&st);
        goto done;
    }

    /* Story 76.1.5: derive source filename and directory for debug metadata */
    const char *dbg_file = src;
    char dbg_dir[PATH_BUF];
    dbg_dir[0] = '\0';
    if (debug_info) {
        const char *slash = strrchr(src, '/');
        if (slash) {
            int dlen = (int)(slash - src);
            if (dlen >= (int)sizeof(dbg_dir)) dlen = (int)sizeof(dbg_dir) - 1;
            memcpy(dbg_dir, src, (size_t)dlen);
            dbg_dir[dlen] = '\0';
            dbg_file = slash + 1;
        } else {
            dbg_dir[0] = '.'; dbg_dir[1] = '\0';
        }
    }

    /* Derive output path */
    char obin[PATH_BUF], ostm[MSG_BUF];
    if (out) {
        strncpy(obin, out, sizeof(obin) - 1); obin[sizeof(obin) - 1] = '\0';
    } else {
        stem(src, ostm, (int)sizeof(ostm));
        snprintf(obin, sizeof(obin), "%s", ostm);
    }

    /* Emit .tki if requested */
    if (emit_iface) {
        char tki[PATH_BUF];
        snprintf(tki, sizeof(tki), "%s.tki", obin);
        if (emit_interface(ast, sbuf, &te, tki) < 0) { symtab_free(&st); rc = EINTERNAL; goto done; }
    }

    /* --emit-tkir: write .tkir binary IR and exit (story 76.1.6a) */
    if (emit_tkir_flag) {
        char tkir_path[PATH_BUF];
        if (out) { strncpy(tkir_path, out, sizeof(tkir_path) - 1); tkir_path[sizeof(tkir_path) - 1] = '\0'; }
        else { snprintf(tkir_path, sizeof(tkir_path), "%s.tkir", obin); }
        if (emit_tkir(ast, sbuf, &te, &ne, tgt, tkir_path) < 0) { symtab_free(&st); rc = EINTERNAL; goto done; }
        progress_update(90);
        symtab_free(&st);
        goto done;
    }

    /* Emit LLVM IR */
    if (emit_ll) {
        /* --emit-llvm: write .ll to output path (or stdout) */
        char ll_path[PATH_BUF];
        if (out) {
            strncpy(ll_path, out, sizeof(ll_path) - 1); ll_path[sizeof(ll_path) - 1] = '\0';
        } else {
            snprintf(ll_path, sizeof(ll_path), "%s.ll", obin);
        }
        CodegenEnv cg = { &te, &ne, arena, tgt, limits, debug_info, dbg_file, dbg_dir };
        if (emit_llvm_ir(ast, sbuf, &cg, ll_path) < 0) { symtab_free(&st); rc = EINTERNAL; goto done; }
        progress_update(90);
        symtab_free(&st);
        goto done;
    }

    if (emit_asm) {
        /* --emit-asm: emit IR to temp, then clang -S to produce .s */
        char tmp[] = "/tmp/tkc_XXXXXX.ll";
        int fd = mkstemps(tmp, 3);
        if (fd < 0) { fputs("tkc: failed to create temp file\n", stderr); symtab_free(&st); rc = EINTERNAL; goto done; }
        close(fd);
        CodegenEnv cg = { &te, &ne, arena, tgt, limits, debug_info, dbg_file, dbg_dir };
        if (emit_llvm_ir(ast, sbuf, &cg, tmp) < 0) { unlink(tmp); symtab_free(&st); rc = EINTERNAL; goto done; }
        progress_update(90);
        char asm_path[PATH_BUF];
        if (out) {
            strncpy(asm_path, out, sizeof(asm_path) - 1); asm_path[sizeof(asm_path) - 1] = '\0';
        } else {
            snprintf(asm_path, sizeof(asm_path), "%s.s", obin);
        }
        char cmd[CMD_BUF];
        if (tgt && tgt[0])
            snprintf(cmd, sizeof cmd, "clang -O%d -Wno-override-module -S -x ir %s -o %s --target=%s 2>&1", opt_level, tmp, asm_path, tgt);
        else
            snprintf(cmd, sizeof cmd, "clang -O%d -Wno-override-module -S -x ir %s -o %s 2>&1", opt_level, tmp, asm_path);
        int r = system(cmd);
        unlink(tmp);
        symtab_free(&st);
        if (r != 0) { rc = EINTERNAL; goto done; }
        goto done;
    }

    /* Default: compile to binary */
    {
        char tmp[] = "/tmp/tkc_XXXXXX.ll";
        int fd = mkstemps(tmp, 3);
        if (fd < 0) { fputs("tkc: failed to create temp file\n", stderr); symtab_free(&st); rc = EINTERNAL; goto done; }
        close(fd);
        CodegenEnv cg = { &te, &ne, arena, tgt, limits, debug_info, dbg_file, dbg_dir };
        if (emit_llvm_ir(ast, sbuf, &cg, tmp) < 0) { unlink(tmp); symtab_free(&st); rc = EINTERNAL; goto done; }
        progress_update(90);
        if (compile_binary(tmp, obin, tgt, opt_level, &st, debug_info) < 0) { unlink(tmp); symtab_free(&st); rc = EINTERNAL; goto done; }
        progress_update(100);
        unlink(tmp);
        symtab_free(&st);
    }

done:
    progress_done();
    diag_flush_sarif();
    arena_free(arena);
    free(sbuf);
    return rc;
}
