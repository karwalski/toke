/*
 * main.c — CLI entry point for the toke reference compiler (tkc).
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
typedef enum { DIAG_FMT_TEXT = 0, DIAG_FMT_JSON = 1 } DiagFormat;
void diag_set_format(DiagFormat fmt);

int lex(const char *src, int src_len, Token *tokens, int token_cap);

/* ── Constants ─────────────────────────────────────────────────────── */

#define VERSION   "tkc 0.1.0 (Phase 1)"
#define EUSAGE    3
#define EINTERNAL 2
#define ECOMPILE  1

static const char HELP[] =
    "Usage: tkc [flags] <source-file>\n"
    "\n"
    "Flags:\n"
    "  --target <triple>   LLVM target triple (default: native)\n"
    "  --out <path>        Output binary path (default: source stem)\n"
    "  --emit-interface    Also emit .tki interface file\n"
    "  --check             Type check only; no code generation\n"
    "  --phase1            Phase 1 character set (default)\n"
    "  --phase2            Phase 2 character set (informational only in v0.1)\n"
    "  --diag-json         Emit diagnostics as JSON (default when not a tty)\n"
    "  --diag-text         Emit diagnostics as human-readable text\n"
    "  --version           Print version and exit\n"
    "  --help              Print this help and exit";

/* ── Utility ───────────────────────────────────────────────────────── */

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

int main(int argc, char **argv)
{
    const char *tgt = NULL, *out = NULL, *src = NULL;
    int emit_iface = 0, check_only = 0, djson = 0, dtext = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version"))        { puts(VERSION); return 0; }
        else if (!strcmp(argv[i], "--help"))       { puts(HELP);   return 0; }
        else if (!strcmp(argv[i], "--emit-interface")) emit_iface = 1;
        else if (!strcmp(argv[i], "--check"))          check_only = 1;
        else if (!strcmp(argv[i], "--phase1"))     { /* default */ }
        else if (!strcmp(argv[i], "--phase2"))     { /* informational */ }
        else if (!strcmp(argv[i], "--diag-json"))  djson = 1;
        else if (!strcmp(argv[i], "--diag-text"))  dtext = 1;
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

    if (!src)            { puts(HELP); return EUSAGE; }
    if (djson && dtext)  { fputs("tkc: --diag-json and --diag-text are mutually exclusive\n", stderr); return EUSAGE; }

    diag_set_format(djson ? DIAG_FMT_JSON :
                    dtext ? DIAG_FMT_TEXT :
                    isatty(STDOUT_FILENO) ? DIAG_FMT_TEXT : DIAG_FMT_JSON);

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
    int tcap = (int)(slen / 2 + 16);
    Token *toks = arena_alloc(arena, tcap * (int)sizeof(Token));
    if (!toks) { rc = EINTERNAL; goto done; }
    int tc = lex(sbuf, (int)slen, toks, tcap);
    if (tc < 0 || diag_error_count() > 0) { rc = ECOMPILE; goto done; }

    /* Parse */
    Node *ast = parse(toks, tc, sbuf, arena);
    if (!ast || diag_error_count() > 0) { rc = ECOMPILE; goto done; }

    /* Resolve imports */
    SymbolTable st;
    if (resolve_imports(ast, sbuf, ".", &st) < 0 || diag_error_count() > 0) { rc = ECOMPILE; goto done; }

    /* Resolve names */
    NameEnv ne;
    if (resolve_names(ast, sbuf, &st, arena, &ne) < 0 || diag_error_count() > 0) {
        symtab_free(&st); rc = ECOMPILE; goto done;
    }

    /* Type check */
    TypeEnv te;
    if (type_check(ast, sbuf, &ne, arena, &te) < 0 || diag_error_count() > 0) {
        symtab_free(&st); rc = ECOMPILE; goto done;
    }

    if (check_only) { symtab_free(&st); goto done; }

    /* Derive output path */
    char obin[272], ostm[256];
    if (out) {
        strncpy(obin, out, sizeof(obin) - 1); obin[sizeof(obin) - 1] = '\0';
    } else {
        stem(src, ostm, (int)sizeof(ostm));
        snprintf(obin, sizeof(obin), "%s", ostm);
    }

    /* Emit .tki if requested */
    if (emit_iface) {
        char tki[280];
        snprintf(tki, sizeof(tki), "%s.tki", obin);
        if (emit_interface(ast, sbuf, &te, tki) < 0) { symtab_free(&st); rc = EINTERNAL; goto done; }
    }

    /* Emit LLVM IR to temp file */
    char tmp[] = "/tmp/tkc_XXXXXX.ll";
    int fd = mkstemps(tmp, 3);
    if (fd < 0) { fputs("tkc: failed to create temp file\n", stderr); symtab_free(&st); rc = EINTERNAL; goto done; }
    close(fd);

    CodegenEnv cg = { &te, &ne, arena, tgt };
    if (emit_llvm_ir(ast, sbuf, &cg, tmp) < 0) { unlink(tmp); symtab_free(&st); rc = EINTERNAL; goto done; }
    if (compile_binary(tmp, obin, tgt)    < 0) { unlink(tmp); symtab_free(&st); rc = EINTERNAL; goto done; }
    unlink(tmp);
    symtab_free(&st);

done:
    arena_free(arena);
    free(sbuf);
    return rc;
}
