/*
 * lexer.c — Lexical analyser for the toke reference compiler.
 *
 * =========================================================================
 * Role in the compiler pipeline
 * =========================================================================
 * The lexer is the first stage of compilation.  It receives raw source text
 * and produces a flat array of Token values that the parser consumes.  No
 * heap allocation occurs; the caller supplies a pre-allocated Token buffer
 * and the lexer fills it sequentially via emit().
 *
 * =========================================================================
 * Character sets
 * =========================================================================
 * Default mode (56-char syntax):
 *   - Letters        a-z  A-Z                (52)
 *   - Digits         0-9                     (10)
 *   - Symbols        ( ) { } [ ] = : . ; + - * / < > ! | " \ $ @ & ^ ~ %   (24)
 *   Keywords are lowercase (m=, f=, t=, i=); uppercase produces E1006.
 *   $ prefixes type references; @ is a valid token.
 *   Uppercase-initial identifiers are plain TK_IDENT (not TK_TYPE_IDENT).
 *
 * Legacy mode (80-char syntax, PROFILE_LEGACY):
 *   - Letters        a-z  A-Z                (52)
 *   - Digits         0-9                     (10)
 *   - Symbols        ( ) { } [ ] = : . ; + - * / < > ! | " \   (18)
 *   Keywords are uppercase (M=, F=, T=, I=).
 *   Uppercase-initial identifiers produce TK_TYPE_IDENT.
 *   $ and @ produce E1003.
 *
 * Any character outside the active set (after whitespace is excluded)
 * triggers diagnostic E1003.
 *
 * =========================================================================
 * Token storage
 * =========================================================================
 * Each Token records:
 *   - kind   — the TokenKind enum value (keyword, literal, symbol, etc.)
 *   - start  — byte offset into the source where the lexeme begins
 *   - len    — length of the lexeme in bytes
 *   - line   — 1-based source line number
 *   - col    — 1-based column number (within the line)
 * Tokens reference the source buffer by offset+length; no copies are made.
 *
 * =========================================================================
 * Error handling strategy (diagnostic codes)
 * =========================================================================
 *   E1001 — Invalid escape sequence in a string literal.
 *   E1002 — Unterminated string literal (EOF before closing quote).
 *   E1003 — Character outside the allowed character set.
 *   E1004 — Digit-starting identifier (e.g. 3abc) or unterminated string
 *           at EOF.
 *   E1005 — Invalid numeric literal (0x/0b with no digits, etc.).
 *   E1006 — Uppercase keyword in default syntax mode.
 *   W1010 — String interpolation \( in legacy profile (not supported).
 *   W1011 — Identifier starts with a keyword prefix (hint).
 *
 * Errors are collected without halting the scan.  After emitting a
 * diagnostic via diag_emit(), the lexer skips past the offending token
 * and continues.  Up to LEX_MAX_ERRORS (20) errors are collected per
 * file; once that limit is reached the scan stops early.  lex() returns
 * -1 if any errors were recorded.  Warnings (W1010, W1011) are
 * non-fatal; lexing continues normally.
 * =========================================================================
 */

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "lexer.h"

/* Diagnostic codes used by the lexer. */
#define LEX_E1001 1001   /* invalid escape sequence                       */
#define LEX_E1002 1002   /* unterminated string literal                   */
#define LEX_E1003 1003   /* character outside Profile 1 set               */
#define LEX_E1004 1004   /* digit-starting identifier / unterminated str  */
#define LEX_E1005 1005   /* invalid numeric literal                       */
#define LEX_E1006 1006   /* uppercase keyword in default syntax mode      */
#define LEX_W1010 1010   /* string interpolation not supported (warning)  */
#define LEX_W1011 1011   /* identifier starts with keyword prefix (hint)  */
#define LEX_W1020 1020   /* foreign keyword detected (cross-lang hint)    */

/* Maximum number of errors before the lexer stops scanning. */
#define LEX_MAX_ERRORS 20

/*
 * Lexer — private scanner state.
 *
 * All fields are value-initialised by lex() before the scan loop begins.
 *   src        — pointer to the source text (not NUL-terminated)
 *   len        — total byte length of `src`
 *   pos        — current read position (byte offset into src)
 *   line       — current 1-based line number
 *   col        — current 1-based column number
 *   out        — caller-supplied token output buffer
 *   out_cap    — maximum number of tokens `out` can hold
 *   out_n      — number of tokens emitted so far
 *   err_count  — number of lexical errors encountered so far
 */
typedef struct {
    const char *src;
    int         len;
    int         pos;
    int         line;
    int         col;
    Token      *out;
    int         out_cap;
    int         out_n;
    Profile     profile;
    int         err_count;
} Lexer;

/* Forward declarations. */
static int  lex_string(Lexer *l, int start, int line, int col);
static int  lex_number(Lexer *l, int start, int line, int col);
static int  lex_ident (Lexer *l, int start, int line, int col);
static void advance(Lexer *l);
static int  emit(Lexer *l, TokenKind k, int start, int len, int line, int col);

/*
 * record_error — Increment the error counter and return 1 if the maximum
 *                has been reached, signalling that the caller should stop.
 */
static int record_error(Lexer *l)
{
    l->err_count++;
    return l->err_count >= LEX_MAX_ERRORS;
}

/* -------------------------------------------------------------------------
 * Character classification helpers
 * ----------------------------------------------------------------------- */

static int is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_ident_cont(char c) { return is_letter(c) || is_digit(c); }
static int is_ident_cont_legacy(char c) { return is_letter(c) || is_digit(c) || c == '_'; }

static int is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* -------------------------------------------------------------------------
 * Keyword table
 * ----------------------------------------------------------------------- */

typedef struct { const char *word; TokenKind kind; } KwEntry;

static const KwEntry KEYWORDS_LEGACY[] = {
    { "F",     TK_KW_F   }, { "T",     TK_KW_T   },
    { "I",     TK_KW_I   }, { "M",     TK_KW_M   },
    { "if",    TK_KW_IF  }, { "el",    TK_KW_EL  },
    { "lp",    TK_KW_LP  }, { "br",    TK_KW_BR  },
    { "let",   TK_KW_LET }, { "mut",   TK_KW_MUT },
    { "as",    TK_KW_AS  }, { "rt",    TK_KW_RT  },
    { "sc",    TK_KW_SC  }, { "mt",    TK_KW_MT  },
    { "true",  TK_BOOL_LIT }, { "false", TK_BOOL_LIT },
};
#define KW_LEGACY_COUNT ((int)(sizeof(KEYWORDS_LEGACY) / sizeof(KEYWORDS_LEGACY[0])))

static const KwEntry KEYWORDS_DEFAULT[] = {
    { "if",    TK_KW_IF  }, { "el",    TK_KW_EL  },
    { "lp",    TK_KW_LP  }, { "br",    TK_KW_BR  },
    { "let",   TK_KW_LET }, { "mut",   TK_KW_MUT },
    { "as",    TK_KW_AS  }, { "rt",    TK_KW_RT  },
    { "sc",    TK_KW_SC  }, { "mt",    TK_KW_MT  },
    { "true",  TK_BOOL_LIT }, { "false", TK_BOOL_LIT },
};
#define KW_DEFAULT_COUNT ((int)(sizeof(KEYWORDS_DEFAULT) / sizeof(KEYWORDS_DEFAULT[0])))

static TokenKind classify_ident(const char *src, int start, int len,
                                Profile profile)
{
    char buf[32];
    int i;
    const KwEntry *table;
    int count;

    if (profile == PROFILE_LEGACY) {
        table = KEYWORDS_LEGACY;
        count = KW_LEGACY_COUNT;
    } else {
        table = KEYWORDS_DEFAULT;
        count = KW_DEFAULT_COUNT;
    }

    if (len >= 32) goto not_kw;
    for (i = 0; i < len; i++) buf[i] = src[start + i];
    buf[len] = '\0';
    for (i = 0; i < count; i++)
        if (strcmp(buf, table[i].word) == 0) return table[i].kind;

not_kw:
    return (src[start] >= 'A' && src[start] <= 'Z') ? TK_TYPE_IDENT : TK_IDENT;
}

/* -------------------------------------------------------------------------
 * advance / emit
 * ----------------------------------------------------------------------- */

static void advance(Lexer *l)
{
    if (l->pos >= l->len) return;
    if (l->src[l->pos] == '\n') { l->line++; l->col = 1; }
    else { l->col++; }
    l->pos++;
}

static int emit(Lexer *l, TokenKind k, int start, int len, int line, int col)
{
    if (l->out_n >= l->out_cap) return -1;
    l->out[l->out_n].kind  = k;
    l->out[l->out_n].start = start;
    l->out[l->out_n].len   = len;
    l->out[l->out_n].line  = line;
    l->out[l->out_n].col   = col;
    l->out_n++;
    return 0;
}

/* -------------------------------------------------------------------------
 * lex_number
 * ----------------------------------------------------------------------- */

static int lex_number(Lexer *l, int start, int line, int col)
{
    int is_float = 0;
    char c = l->src[l->pos];
    if (c == '0' && l->pos + 1 < l->len) {
        char next = l->src[l->pos + 1];
        if (next == 'x' || next == 'X') {
            advance(l); advance(l);
            if (l->pos >= l->len || !is_hex(l->src[l->pos])) {
                diag_emit(DIAG_ERROR, LEX_E1005, start, line, col,
                          "invalid numeric literal: 0x requires hex digits",
                          "fix", "provide hex digits after 0x (e.g., 0xFF)",
                          "got", "0x with no hex digits",
                          "expected", "hexadecimal digit (0-9, a-f)", NULL);
                return -1;
            }
            while (l->pos < l->len && is_hex(l->src[l->pos])) advance(l);
            if (l->pos < l->len && is_letter(l->src[l->pos])) {
                while (l->pos < l->len && is_ident_cont(l->src[l->pos])) advance(l);
                diag_emit(DIAG_ERROR, LEX_E1004, start, line, col,
                          "identifier must not start with a digit",
                          "fix", "identifiers must start with a-z or A-Z",
                          "expected", "letter (a-z, A-Z) to start an identifier", NULL);
                return -1;
            }
            return emit(l, TK_INT_LIT, start, l->pos - start, line, col);
        }
        if (next == 'b' || next == 'B') {
            advance(l); advance(l);
            if (l->pos >= l->len || (l->src[l->pos] != '0' && l->src[l->pos] != '1')) {
                diag_emit(DIAG_ERROR, LEX_E1005, start, line, col,
                          "invalid numeric literal: 0b requires binary digits",
                          "fix", "provide binary digits after 0b (e.g., 0b1010)",
                          "got", "0b with no binary digits",
                          "expected", "binary digit (0 or 1)", NULL);
                return -1;
            }
            while (l->pos < l->len &&
                   (l->src[l->pos] == '0' || l->src[l->pos] == '1')) advance(l);
            if (l->pos < l->len && is_letter(l->src[l->pos])) {
                while (l->pos < l->len && is_ident_cont(l->src[l->pos])) advance(l);
                diag_emit(DIAG_ERROR, LEX_E1004, start, line, col,
                          "identifier must not start with a digit",
                          "fix", "identifiers must start with a-z or A-Z",
                          "expected", "letter (a-z, A-Z) to start an identifier", NULL);
                return -1;
            }
            return emit(l, TK_INT_LIT, start, l->pos - start, line, col);
        }
    }
    while (l->pos < l->len && is_digit(l->src[l->pos])) advance(l);
    if (l->pos < l->len && l->src[l->pos] == '.' &&
        l->pos + 1 < l->len && is_digit(l->src[l->pos + 1])) {
        is_float = 1;
        advance(l);
        while (l->pos < l->len && is_digit(l->src[l->pos])) advance(l);
    }
    /* Detect digit-starting identifiers: e.g. "3abc", "42xyz". */
    if (l->pos < l->len && is_letter(l->src[l->pos])) {
        while (l->pos < l->len && is_ident_cont(l->src[l->pos])) advance(l);
        diag_emit(DIAG_ERROR, LEX_E1004, start, line, col,
                  "identifier must not start with a digit", NULL);
        return -1;
    }
    return emit(l, is_float ? TK_FLOAT_LIT : TK_INT_LIT,
                start, l->pos - start, line, col);
}

/* -------------------------------------------------------------------------
 * lex_string
 * ----------------------------------------------------------------------- */

static int lex_string(Lexer *l, int start, int line, int col)
{
    int had_error = 0;

    while (l->pos < l->len) {
        char c = l->src[l->pos];
        if (c == '"') {
            advance(l);
            if (!had_error)
                return emit(l, TK_STR_LIT, start, l->pos - start, line, col);
            return -1;
        }
        if (c == '\\') {
            advance(l);
            if (l->pos >= l->len) break;
            char esc = l->src[l->pos];
            if (esc == '"' || esc == '\\' || esc == 'n' ||
                esc == 't' || esc == 'r'  || esc == '0') {
                advance(l);
            } else if (esc == 'x') {
                advance(l);
                if (l->pos + 1 < l->len &&
                    is_hex(l->src[l->pos]) && is_hex(l->src[l->pos + 1])) {
                    advance(l); advance(l);
                } else {
                    diag_emit(DIAG_ERROR, LEX_E1001, l->pos, l->line, l->col,
                              "invalid escape sequence: \\x requires two hex digits",
                              "fix", "use \\xHH with exactly two hex digits (e.g., \\x0A)",
                              "got", "\\x without two hex digits",
                              "expected", "\\xHH (two hex digits 0-9, a-f)", NULL);
                    had_error = 1;
                    if (record_error(l)) return -1;
                }
            } else if (esc == '(') {
                if (l->profile == PROFILE_LEGACY) {
                    diag_emit(DIAG_WARNING, LEX_W1010, l->pos - 1, l->line, l->col - 1,
                              "string interpolation \\( is not supported in Profile 1; "
                              "use str.concat() instead",
                              "fix", "use str.concat() for string composition", NULL);
                }
                advance(l);
                int depth = 1;
                while (l->pos < l->len && depth > 0) {
                    if      (l->src[l->pos] == '(') depth++;
                    else if (l->src[l->pos] == ')') depth--;
                    advance(l);
                }
            } else {
                char esc_got[8];
                snprintf(esc_got, sizeof(esc_got), "\\%c", esc);
                char esc_msg[128];
                snprintf(esc_msg, sizeof(esc_msg),
                         "invalid escape sequence '\\%c' in string literal", esc);
                diag_emit(DIAG_ERROR, LEX_E1001, l->pos - 1, l->line, l->col - 1,
                          esc_msg,
                          "fix", "valid escapes are: \\n \\t \\r \\\\ \\\" \\0 \\xHH",
                          "got", esc_got,
                          "expected", "one of: \\n \\t \\r \\\\ \\\" \\0 \\xHH", NULL);
                had_error = 1;
                if (record_error(l)) return -1;
                /* Skip the bad escape character and continue scanning */
                advance(l);
            }
        } else {
            advance(l);
        }
    }
    diag_emit(DIAG_ERROR, LEX_E1004, start, line, col,
              "unterminated string literal at end of file",
              "fix", "add closing double-quote '\"' to terminate the string",
              "got", "end of file",
              "expected", "closing '\"'", NULL);
    record_error(l);
    return -1;
}

/* -------------------------------------------------------------------------
 * lex_ident
 * ----------------------------------------------------------------------- */

typedef struct { char upper; const char *lower; } UpperKwEntry;
static const UpperKwEntry UPPER_KW_MAP[] = {
    { 'M', "m=" }, { 'F', "f=" }, { 'T', "t=" },
    { 'I', "i=" }, { 'C', "c=" },
};
#define UPPER_KW_COUNT ((int)(sizeof(UPPER_KW_MAP) / sizeof(UPPER_KW_MAP[0])))

typedef struct { const char *python_kw; const char *message; const char *fix; } PyKwEntry;

static const PyKwEntry PYTHON_KEYWORDS[] = {
    { "def",    "Python keyword 'def' detected; toke uses f=name():type{body}",
                "replace 'def name(args):' with 'f=name(args):type{'" },
    { "return", "Python keyword 'return' detected; toke uses <expr for returns",
                "replace 'return expr' with '<expr'" },
    { "import", "Python keyword 'import' detected; toke uses i=alias:std.module;",
                "replace 'import module' with 'i=alias:std.module;'" },
    { "class",  "Python keyword 'class' detected; toke uses t=$typename{fields}",
                "replace 'class Name:' with 't=$typename{fields}'" },
    { "elif",   "Python keyword 'elif' detected; toke uses el{if(cond){",
                "replace 'elif cond:' with 'el{if(cond){'" },
    { "print",  "Python keyword 'print' detected; toke uses io.println()",
                "replace 'print(...)' with 'io.println(...)'" },
    { "None",   "Python keyword 'None' detected; toke uses 0 or struct with empty field",
                "replace 'None' with '0' or an empty-field struct" },
};
#define PY_KW_COUNT ((int)(sizeof(PYTHON_KEYWORDS) / sizeof(PYTHON_KEYWORDS[0])))

static int check_python_keyword(const char *buf, int start, int line, int col)
{
    int i;
    for (i = 0; i < PY_KW_COUNT; i++) {
        if (strcmp(buf, PYTHON_KEYWORDS[i].python_kw) == 0) {
            diag_emit(DIAG_WARNING, LEX_W1020, start, line, col,
                      PYTHON_KEYWORDS[i].message,
                      "fix", PYTHON_KEYWORDS[i].fix, NULL);
            return 1;
        }
    }
    if (strcmp(buf, "True") == 0) {
        diag_emit(DIAG_WARNING, LEX_W1020, start, line, col,
                  "Python boolean 'True' detected; toke uses 'true'",
                  "fix", "replace 'True' with 'true'", NULL);
        return 1;
    }
    if (strcmp(buf, "False") == 0) {
        diag_emit(DIAG_WARNING, LEX_W1020, start, line, col,
                  "Python boolean 'False' detected; toke uses 'false'",
                  "fix", "replace 'False' with 'false'", NULL);
        return 1;
    }
    return 0;
}

static int lex_ident(Lexer *l, int start, int line, int col)
{
    while (l->pos < l->len && is_ident_cont(l->src[l->pos])) advance(l);

    /* Detect v0.2 underscore identifiers */
    if (l->profile == PROFILE_DEFAULT && l->pos < l->len && l->src[l->pos] == '_') {
        while (l->pos < l->len && is_ident_cont_legacy(l->src[l->pos])) advance(l);
        int full_len = l->pos - start;
        char id_buf[128];
        int cplen = full_len < (int)sizeof(id_buf) - 1 ? full_len : (int)sizeof(id_buf) - 1;
        for (int j = 0; j < cplen; j++) id_buf[j] = l->src[start + j];
        id_buf[cplen] = '\0';
        char msg[256];
        snprintf(msg, sizeof msg,
                 "identifier '%s' contains underscore (v0.2 syntax); "
                 "run `toke --migrate` to convert to v0.3", id_buf);
        diag_emit(DIAG_ERROR, LEX_E1003, start, line, col, msg,
                  "fix", "remove underscores: concatenate words (e.g., to_int -> toint)",
                  "got", id_buf,
                  "expected", "identifier without underscores (a-z, A-Z, 0-9)", NULL);
        return -1;
    }

    int len = l->pos - start;
    TokenKind kind = classify_ident(l->src, start, len, l->profile);

    /* Reject uppercase single-letter declaration keywords in default mode */
    if (l->profile == PROFILE_DEFAULT && len == 1) {
        char ch = l->src[start];
        if (ch >= 'A' && ch <= 'Z' && l->pos < l->len && l->src[l->pos] == '=') {
            for (int i = 0; i < UPPER_KW_COUNT; i++) {
                if (ch == UPPER_KW_MAP[i].upper) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "uppercase keyword `%c=` is not valid in default "
                             "syntax mode — use `%s` instead",
                             ch, UPPER_KW_MAP[i].lower);
                    {
                    char got_kw[4], fix_kw[64];
                    snprintf(got_kw, sizeof(got_kw), "%c=", ch);
                    snprintf(fix_kw, sizeof(fix_kw),
                             "replace '%c=' with '%s'", ch, UPPER_KW_MAP[i].lower);
                    diag_emit(DIAG_ERROR, LEX_E1006, start, line, col,
                              msg, "fix", fix_kw,
                              "got", got_kw,
                              "expected", UPPER_KW_MAP[i].lower, NULL);
                    }
                    return -1;
                }
            }
        }
    }

    /* W1020: detect Python keywords and emit migration hints. */
    if (kind == TK_IDENT || kind == TK_TYPE_IDENT) {
        char py_buf[32];
        int py_len = len < (int)sizeof(py_buf) - 1 ? len : (int)sizeof(py_buf) - 1;
        for (int pi = 0; pi < py_len; pi++) py_buf[pi] = l->src[start + pi];
        py_buf[py_len] = '\0';
        check_python_keyword(py_buf, start, line, col);
    }

    return emit(l, kind, start, len, line, col);
}

/* -------------------------------------------------------------------------
 * lex — public entry point (multi-error recovery)
 * ----------------------------------------------------------------------- */

int lex(const char *src, int src_len, Token *out, int out_cap, Profile profile)
{
    Lexer l;
    l.src       = src;
    l.len       = src_len;
    l.pos       = 0;
    l.line      = 1;
    l.col       = 1;
    l.out       = out;
    l.out_cap   = out_cap;
    l.out_n     = 0;
    l.profile   = profile;
    l.err_count = 0;

    while (l.pos < l.len) {
        char c     = src[l.pos];
        int  start = l.pos;
        int  line  = l.line;
        int  col   = l.col;

        /* 1. Skip whitespace silently. */
        if (is_whitespace(c)) { advance(&l); continue; }

        /* 2. Identifiers and keywords */
        if (c == '_' && l.profile == PROFILE_DEFAULT) {
            advance(&l);
            while (l.pos < l.len && is_ident_cont_legacy(l.src[l.pos])) advance(&l);
            int ulen = l.pos - start;
            char ubuf[128]; int cp = ulen < (int)sizeof(ubuf)-1 ? ulen : (int)sizeof(ubuf)-1;
            for (int j = 0; j < cp; j++) ubuf[j] = l.src[start+j]; ubuf[cp] = '\0';
            char umsg[256];
            snprintf(umsg, sizeof umsg,
                     "identifier '%s' starts with underscore (v0.2 syntax); "
                     "run `toke --migrate` to convert to v0.3", ubuf);
            diag_emit(DIAG_ERROR, LEX_E1003, start, line, col, umsg,
                      "fix", "remove underscores: identifiers must start with a letter (a-z, A-Z)",
                      "got", ubuf,
                      "expected", "identifier starting with a letter (a-z, A-Z)", NULL);
            if (record_error(&l)) break;
            continue;
        }
        if (is_letter(c) || (c == '_' && l.profile == PROFILE_LEGACY)) {
            advance(&l);
            if (lex_ident(&l, start, line, col) < 0) {
                if (record_error(&l)) break;
            }
            continue;
        }
        /* 3. Numeric literals */
        if (is_digit(c)) {
            if (lex_number(&l, start, line, col) < 0) {
                if (record_error(&l)) break;
            }
            continue;
        }
        /* 4. String literals */
        if (c == '"') {
            advance(&l);
            if (lex_string(&l, start, line, col) < 0) {
                if (l.err_count >= LEX_MAX_ERRORS) break;
            }
            continue;
        }

        /* 4b. Python-style '#' line comments */
        if (c == '#') {
            diag_emit(DIAG_WARNING, LEX_W1020, start, line, col,
                      "Python comment '#' detected; toke uses (* comment *) block comments",
                      "fix", "replace '# comment' with '(* comment *)'", NULL);
            while (l.pos < l.len && l.src[l.pos] != '\n') advance(&l);
            continue;
        }

        /* 5. Single-character symbols. */
        TokenKind sym;
        int sym_error = 0;
        switch (c) {
        case '(':
            if (l.pos + 1 < l.len && src[l.pos + 1] == '*') {
                advance(&l); advance(&l);
                int depth = 1;
                while (l.pos < l.len && depth > 0) {
                    if (l.pos + 1 < l.len &&
                        src[l.pos] == '(' && src[l.pos + 1] == '*') {
                        advance(&l); advance(&l);
                        depth++;
                    } else if (l.pos + 1 < l.len &&
                               src[l.pos] == '*' && src[l.pos + 1] == ')') {
                        advance(&l); advance(&l);
                        depth--;
                    } else {
                        advance(&l);
                    }
                }
                continue;
            }
            sym = TK_LPAREN; break;
        case ')':  sym = TK_RPAREN;   break;
        case '{':  sym = TK_LBRACE;   break;
        case '}':  sym = TK_RBRACE;   break;
        case '[':
            if (l.profile == PROFILE_DEFAULT) {
                diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                          "square brackets are not allowed in default mode; "
                          "use @() for arrays/maps",
                          "fix", "replace '[' with '@(' for array/map literals",
                          "got", "[",
                          "expected", "@( for arrays/maps", NULL);
                sym_error = 1;
                if (record_error(&l)) break;
                advance(&l);
                continue;
            }
            sym = TK_LBRACKET; break;
        case ']':
            if (l.profile == PROFILE_DEFAULT) {
                diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                          "square brackets are not allowed in default mode; "
                          "use @() for arrays/maps",
                          "fix", "replace ']' with ')' to close @() array/map literal",
                          "got", "]",
                          "expected", ") to close @() array/map", NULL);
                sym_error = 1;
                if (record_error(&l)) break;
                advance(&l);
                continue;
            }
            sym = TK_RBRACKET; break;
        case '=':  sym = TK_EQ;       break;
        case ':':  sym = TK_COLON;    break;
        case '.':  sym = TK_DOT;      break;
        case ';':  sym = TK_SEMI;     break;
        case '+':  sym = TK_PLUS;     break;
        case '-':  sym = TK_MINUS;    break;
        case '*':  sym = TK_STAR;     break;
        case '/':  sym = TK_SLASH;    break;
        case '<':
            if (l.pos + 1 < l.len && src[l.pos + 1] == '<') {
                if (l.profile == PROFILE_DEFAULT) {
                    diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                              "shift operator '<<' not available in v0.3 (deferred to v0.5); "
                              "run `toke --migrate` to update",
                              "fix", "remove '<<'; bitwise shift is deferred to v0.5",
                              "got", "<<", NULL);
                    sym_error = 1;
                    if (record_error(&l)) break;
                    advance(&l); advance(&l);
                    continue;
                }
                advance(&l); advance(&l);
                if (emit(&l, TK_SHL, start, 2, line, col) < 0) return -1;
                continue;
            }
            sym = TK_LT; break;
        case '>':
            if (l.pos + 1 < l.len && src[l.pos + 1] == '>') {
                if (l.profile == PROFILE_DEFAULT) {
                    diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                              "shift operator '>>' not available in v0.3 (deferred to v0.5); "
                              "run `toke --migrate` to update",
                              "fix", "remove '>>'; bitwise shift is deferred to v0.5",
                              "got", ">>", NULL);
                    sym_error = 1;
                    if (record_error(&l)) break;
                    advance(&l); advance(&l);
                    continue;
                }
                advance(&l); advance(&l);
                if (emit(&l, TK_SHR, start, 2, line, col) < 0) return -1;
                continue;
            }
            sym = TK_GT; break;
        case '!':  sym = TK_BANG;     break;
        case '|':
            if (l.pos + 1 < l.len && src[l.pos + 1] == '|') {
                advance(&l); advance(&l);
                if (emit(&l, TK_OR, start, 2, line, col) < 0) return -1;
                continue;
            }
            sym = TK_PIPE; break;
        case '&':
            if (l.pos + 1 < l.len && src[l.pos + 1] == '&') {
                advance(&l); advance(&l);
                if (emit(&l, TK_AND, start, 2, line, col) < 0) return -1;
                continue;
            }
            sym = TK_AMP; break;
        case '^':
            if (l.profile == PROFILE_DEFAULT) {
                diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                          "bitwise XOR '^' not available in v0.3 (deferred to v0.5); "
                          "run `toke --migrate` to update",
                          "fix", "remove '^'; bitwise XOR is deferred to v0.5",
                          "got", "^", NULL);
                sym_error = 1;
                if (record_error(&l)) break;
                advance(&l);
                continue;
            }
            sym = TK_CARET; break;
        case '~':
            if (l.profile == PROFILE_DEFAULT) {
                diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                          "bitwise NOT '~' not available in v0.3 (deferred to v0.5); "
                          "run `toke --migrate` to update",
                          "fix", "remove '~'; bitwise NOT is deferred to v0.5",
                          "got", "~", NULL);
                sym_error = 1;
                if (record_error(&l)) break;
                advance(&l);
                continue;
            }
            sym = TK_TILDE; break;
        case '%':  sym = TK_PERCENT;  break;
        case '$':
            if (l.profile == PROFILE_LEGACY) {
                diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                          "character outside Profile 1 character set",
                          "fix", "remove '$'; type references use uppercase names in legacy mode",
                          "got", "$", NULL);
                sym_error = 1;
                if (record_error(&l)) break;
                advance(&l);
                continue;
            }
            sym = TK_DOLLAR; break;
        case '@':
            if (l.profile == PROFILE_LEGACY) {
                diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                          "character outside Profile 1 character set",
                          "fix", "remove '@'; use --default mode for @() array/map syntax",
                          "got", "@", NULL);
                sym_error = 1;
                if (record_error(&l)) break;
                advance(&l);
                continue;
            }
            sym = TK_AT; break;
        default: {
            char got_ch[8];
            if (c >= 0x20 && c <= 0x7E)
                snprintf(got_ch, sizeof(got_ch), "%c", c);
            else
                snprintf(got_ch, sizeof(got_ch), "0x%02X", (unsigned char)c);
            diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                      "character outside allowed character set",
                      "fix", "remove this character; only ASCII letters, digits, and toke operators are allowed",
                      "got", got_ch,
                      "expected", "ASCII letter, digit, or toke operator", NULL);
            sym_error = 1;
            if (record_error(&l)) break;
            advance(&l);
            continue;
        }
        }
        if (sym_error) break;
        advance(&l);
        if (emit(&l, sym, start, 1, line, col) < 0) return -1;
    }

    /* Append the EOF sentinel so the parser always has a clean terminator. */
    if (emit(&l, TK_EOF, l.pos, 0, l.line, l.col) < 0) return -1;

    /* If any errors were recorded, return -1 to signal failure. */
    if (l.err_count > 0) return -1;
    return l.out_n;
}
