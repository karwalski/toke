/*
 * lexer.c — Lexical analyser for the toke Profile 1 reference compiler.
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
 * Profile 1 character set (80 characters)
 * =========================================================================
 * toke Profile 1 restricts source files to an 80-character repertoire:
 *   - Letters        a-z  A-Z                (52)
 *   - Digits         0-9                     (10)
 *   - Symbols        ( ) { } [ ] = : . ; + - * / < > ! | " \   (18)
 * Any character outside this set (after whitespace is excluded) triggers
 * diagnostic E1003.  Whitespace characters (space, tab, CR, LF) are
 * consumed silently and never produce tokens.
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
 *   E1003 — Character outside the Profile 1 character set.
 *   W1010 — String interpolation \( attempted (not supported in Profile 1).
 *
 * Errors cause lex() to return -1 immediately after emitting the
 * diagnostic via diag_emit().  The W1010 warning is non-fatal; lexing
 * continues after skipping the interpolation expression.
 * =========================================================================
 */

#include <string.h>

#include "diag.h"
#include "lexer.h"

/* Diagnostic codes used by the lexer. */
#define LEX_E1001 1001   /* invalid escape sequence                       */
#define LEX_E1002 1002   /* unterminated string literal                   */
#define LEX_E1003 1003   /* character outside Profile 1 set               */
#define LEX_W1010 1010   /* string interpolation not supported (warning)  */

/*
 * Lexer — private scanner state.
 *
 * All fields are value-initialised by lex() before the scan loop begins.
 *   src     — pointer to the source text (not NUL-terminated)
 *   len     — total byte length of `src`
 *   pos     — current read position (byte offset into src)
 *   line    — current 1-based line number
 *   col     — current 1-based column number
 *   out     — caller-supplied token output buffer
 *   out_cap — maximum number of tokens `out` can hold
 *   out_n   — number of tokens emitted so far
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
} Lexer;

/* Forward declarations. */
static int  lex_string(Lexer *l, int start, int line, int col);
static int  lex_number(Lexer *l, int start, int line, int col);
static int  lex_ident (Lexer *l, int start, int line, int col);
static void advance(Lexer *l);
static int  emit(Lexer *l, TokenKind k, int start, int len, int line, int col);

/* -------------------------------------------------------------------------
 * Character classification helpers
 * -------------------------------------------------------------------------
 * These small predicates are used throughout the scanner to classify the
 * current character without repeating range checks inline.  They operate
 * on ASCII values only; multi-byte encodings are not relevant because
 * Profile 1 is pure ASCII.
 * ----------------------------------------------------------------------- */

/*
 * is_letter — Return non-zero if `c` is an ASCII letter (a-z, A-Z).
 */
static int is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/*
 * is_digit — Return non-zero if `c` is an ASCII decimal digit (0-9).
 */
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/*
 * is_ident_cont — Return non-zero if `c` may appear as the second or
 * subsequent character of an identifier (letter or digit).
 *
 * Note: toke identifiers may not contain underscores.
 */
static int is_ident_cont(char c) { return is_letter(c) || is_digit(c); }

/*
 * is_hex — Return non-zero if `c` is a valid hexadecimal digit
 * (0-9, a-f, A-F).  Used when parsing 0x... integer literals and
 * \xHH escape sequences inside strings.
 */
static int is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/*
 * is_whitespace — Return non-zero if `c` is a whitespace character that
 * the lexer should silently skip (space, tab, carriage return, newline).
 */
static int is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* -------------------------------------------------------------------------
 * Keyword table
 * -------------------------------------------------------------------------
 * The 12 toke keywords plus the two boolean literals are stored in a flat
 * table.  classify_ident() performs a linear scan — the table is small
 * enough (14 entries) that a hash map would add complexity without a
 * measurable speed-up.
 *
 * Note that "true" and "false" map to TK_BOOL_LIT, not to keyword tokens.
 * ----------------------------------------------------------------------- */

typedef struct { const char *word; TokenKind kind; } KwEntry;
static const KwEntry KEYWORDS[] = {
    { "F",     TK_KW_F   }, { "T",     TK_KW_T   },
    { "I",     TK_KW_I   }, { "M",     TK_KW_M   },
    { "if",    TK_KW_IF  }, { "el",    TK_KW_EL  },
    { "lp",    TK_KW_LP  }, { "br",    TK_KW_BR  },
    { "let",   TK_KW_LET }, { "mut",   TK_KW_MUT },
    { "as",    TK_KW_AS  }, { "rt",    TK_KW_RT  },
    { "true",  TK_BOOL_LIT }, { "false", TK_BOOL_LIT },
};
#define KW_COUNT ((int)(sizeof(KEYWORDS) / sizeof(KEYWORDS[0])))

/*
 * classify_ident — Determine whether a scanned identifier is a keyword,
 *                  boolean literal, type identifier, or plain identifier.
 *
 * Parameters:
 *   src   — full source text
 *   start — byte offset where the identifier begins
 *   len   — length of the identifier in bytes
 *
 * Returns:
 *   The appropriate TokenKind.  Lookup proceeds as follows:
 *     1. If the identifier is >= 32 characters it cannot be a keyword
 *        (all keywords are <= 5 chars), so skip the table search.
 *     2. Copy the identifier into a NUL-terminated local buffer and
 *        compare against every entry in KEYWORDS[].
 *     3. If no keyword matches, inspect the first character: an uppercase
 *        letter means the token is a type identifier (TK_TYPE_IDENT),
 *        otherwise it is a plain identifier (TK_IDENT).
 *
 * Non-obvious logic — prefix disambiguation:
 *   Because keywords like "lp" and "let" share a common prefix, the
 *   lexer does NOT try to match keywords character-by-character during
 *   scanning.  Instead, lex_ident() consumes the full run of identifier
 *   characters first, then classify_ident() matches the complete string
 *   against the keyword table.  This avoids ambiguity: "lp" matches
 *   TK_KW_LP while "let" matches TK_KW_LET, and "lpx" matches TK_IDENT.
 */
static TokenKind classify_ident(const char *src, int start, int len)
{
    char buf[32];
    int i;
    if (len >= 32) goto not_kw;
    for (i = 0; i < len; i++) buf[i] = src[start + i];
    buf[len] = '\0';
    for (i = 0; i < KW_COUNT; i++)
        if (strcmp(buf, KEYWORDS[i].word) == 0) return KEYWORDS[i].kind;
not_kw:
    return (src[start] >= 'A' && src[start] <= 'Z') ? TK_TYPE_IDENT : TK_IDENT;
}

/*
 * advance — Move the scanner forward by one byte, updating position
 *           tracking (line and column numbers).
 *
 * Parameters:
 *   l — lexer state
 *
 * Invariants:
 *   - If l->pos >= l->len the call is a no-op (prevents reading past the
 *     source buffer).
 *   - On a newline character the line counter increments and the column
 *     resets to 1 BEFORE pos is incremented, so the newline itself is
 *     reported at its original position.
 *   - Column is incremented for every non-newline character.
 */
static void advance(Lexer *l)
{
    if (l->pos >= l->len) return;
    if (l->src[l->pos] == '\n') { l->line++; l->col = 1; }
    else { l->col++; }
    l->pos++;
}

/*
 * emit — Append a token to the output buffer.
 *
 * Parameters:
 *   l     — lexer state (contains the output buffer and its capacity)
 *   k     — token kind to record
 *   start — byte offset in the source where this token begins
 *   len   — length of the token's lexeme in bytes
 *   line  — 1-based line number where the token starts
 *   col   — 1-based column number where the token starts
 *
 * Returns:
 *    0 on success.
 *   -1 if the output buffer is full (l->out_n >= l->out_cap).
 *
 * Invariants:
 *   - On success, l->out_n is incremented by exactly 1.
 *   - The token is stored at l->out[l->out_n] (the old value) before
 *     the counter advances.
 */
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

/*
 * lex_number — Scan a numeric literal starting at the current position.
 *
 * Parameters:
 *   l     — lexer state (l->pos points to the first digit)
 *   start — byte offset where this number token begins (same as l->pos
 *           on entry, captured by the caller before any advance)
 *   line  — line number at the start of the token
 *   col   — column number at the start of the token
 *
 * Returns:
 *    0 on success (token emitted).
 *   -1 if the output buffer overflows.
 *
 * Supported formats:
 *   - Decimal integer:   one or more digits, e.g. "42"
 *   - Hexadecimal:       "0x" or "0X" prefix followed by hex digits
 *   - Binary:            "0b" or "0B" prefix followed by '0' and '1'
 *   - Floating-point:    digits, a dot, then more digits, e.g. "3.14"
 *
 * Non-obvious logic — float detection:
 *   A dot is only treated as a decimal point if the character immediately
 *   after the dot is a digit.  This prevents "42.method" from being
 *   mis-parsed as a float; the lexer would emit TK_INT_LIT for "42",
 *   then TK_DOT, then the identifier "method" in the main loop.
 */
static int lex_number(Lexer *l, int start, int line, int col)
{
    int is_float = 0;
    char c = l->src[l->pos];
    if (c == '0' && l->pos + 1 < l->len) {
        char next = l->src[l->pos + 1];
        if (next == 'x' || next == 'X') {
            advance(l); advance(l);
            while (l->pos < l->len && is_hex(l->src[l->pos])) advance(l);
            return emit(l, TK_INT_LIT, start, l->pos - start, line, col);
        }
        if (next == 'b' || next == 'B') {
            advance(l); advance(l);
            while (l->pos < l->len &&
                   (l->src[l->pos] == '0' || l->src[l->pos] == '1')) advance(l);
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
    return emit(l, is_float ? TK_FLOAT_LIT : TK_INT_LIT,
                start, l->pos - start, line, col);
}

/*
 * lex_string — Scan a string literal, handling escape sequences.
 *
 * Parameters:
 *   l     — lexer state; l->pos points to the character immediately AFTER
 *           the opening double-quote (the caller already advanced past it)
 *   start — byte offset of the opening '"' character
 *   line  — line number at the opening '"'
 *   col   — column number at the opening '"'
 *
 * Returns:
 *    0 on success (TK_STR_LIT emitted; the token span includes both
 *      the opening and closing quotes).
 *   -1 on error (diagnostic emitted; the caller should propagate -1).
 *
 * Supported escape sequences:
 *   \"  \\  \n  \t  \r  \0  — single-character escapes
 *   \xHH                   — hex escape (exactly two hex digits required)
 *
 * Non-obvious logic — string interpolation (\():
 *   toke defines \( ... ) as string interpolation syntax, but Profile 1
 *   does NOT support it.  Instead of producing an error, the lexer emits
 *   a W1010 warning with a "fix" hint and then skips the interpolation
 *   expression by tracking parenthesis nesting depth.  This allows the
 *   rest of the string to be scanned normally, producing better
 *   downstream error recovery.
 *
 * Error conditions:
 *   - Backslash followed by an unrecognised character -> E1001.
 *   - \x not followed by exactly two hex digits       -> E1001.
 *   - EOF reached before a closing '"'                -> E1002.
 */
static int lex_string(Lexer *l, int start, int line, int col)
{
    while (l->pos < l->len) {
        char c = l->src[l->pos];
        if (c == '"') {
            advance(l);
            return emit(l, TK_STR_LIT, start, l->pos - start, line, col);
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
                              NULL);
                    return -1;
                }
            } else if (esc == '(') {
                diag_emit(DIAG_WARNING, LEX_W1010, l->pos - 1, l->line, l->col - 1,
                          "string interpolation \\( is not supported in Profile 1; "
                          "use str.concat() instead",
                          "fix", "use str.concat() for string composition", NULL);
                advance(l);
                int depth = 1;
                while (l->pos < l->len && depth > 0) {
                    if      (l->src[l->pos] == '(') depth++;
                    else if (l->src[l->pos] == ')') depth--;
                    advance(l);
                }
            } else {
                diag_emit(DIAG_ERROR, LEX_E1001, l->pos - 1, l->line, l->col - 1,
                          "invalid escape sequence in string literal", NULL);
                return -1;
            }
        } else {
            advance(l);
        }
    }
    diag_emit(DIAG_ERROR, LEX_E1002, start, line, col,
              "unterminated string literal", NULL);
    return -1;
}

/*
 * lex_ident — Scan the remainder of an identifier or keyword.
 *
 * Parameters:
 *   l     — lexer state; l->pos points to the SECOND character of the
 *           identifier (the main loop already advanced past the first
 *           letter)
 *   start — byte offset of the first character
 *   line  — line number at the first character
 *   col   — column number at the first character
 *
 * Returns:
 *    0 on success (token emitted).
 *   -1 if the output buffer overflows.
 *
 * After consuming all contiguous identifier-continuation characters
 * (letters and digits), the full lexeme is passed to classify_ident()
 * which decides whether it is a keyword, boolean literal, type
 * identifier, or plain identifier.
 */
static int lex_ident(Lexer *l, int start, int line, int col)
{
    while (l->pos < l->len && is_ident_cont(l->src[l->pos])) advance(l);
    int len = l->pos - start;
    return emit(l, classify_ident(l->src, start, len), start, len, line, col);
}

/*
 * lex — Tokenise a source buffer into a caller-supplied Token array.
 *
 * This is the public entry point for the lexer.
 *
 * Parameters:
 *   src     — pointer to the source text (need not be NUL-terminated)
 *   src_len — number of bytes in `src`
 *   out     — pre-allocated array of Token structs to fill
 *   out_cap — capacity of `out` (maximum tokens it can hold)
 *
 * Returns:
 *   On success: the number of tokens written to `out`, including the
 *               trailing TK_EOF sentinel.
 *   On error:   -1 (a diagnostic has already been emitted via diag_emit).
 *
 * Invariants:
 *   - The returned token count is always >= 1 on success (at minimum the
 *     TK_EOF token is emitted).
 *   - Tokens are emitted in source order; their `start` fields are
 *     monotonically non-decreasing.
 *   - No heap allocation is performed.  The function is safe to call from
 *     any context where `out` has been pre-allocated.
 *
 * Main loop structure:
 *   1. Skip whitespace.
 *   2. If the character is a letter  -> lex_ident (keywords resolved
 *      after the full identifier is consumed).
 *   3. If the character is a digit   -> lex_number.
 *   4. If the character is '"'       -> lex_string.
 *   5. Otherwise, look up the character in the single-char symbol table
 *      (the switch statement).  If it is not recognised, emit E1003.
 *
 * Non-obvious logic — advance before lex_ident / lex_string:
 *   For identifiers and strings, the main loop calls advance() to move
 *   past the initial character (the first letter, or the opening '"')
 *   before delegating to the sub-scanner.  The sub-scanners therefore
 *   begin with l->pos pointing at the SECOND character of the lexeme.
 *   The `start` parameter preserves the original offset so the token
 *   span remains correct.  lex_number does NOT follow this pattern —
 *   it begins with l->pos still on the first digit, because the leading
 *   '0' may need to be inspected for prefix detection (0x, 0b).
 */
int lex(const char *src, int src_len, Token *out, int out_cap)
{
    Lexer l;
    l.src     = src;
    l.len     = src_len;
    l.pos     = 0;
    l.line    = 1;
    l.col     = 1;
    l.out     = out;
    l.out_cap = out_cap;
    l.out_n   = 0;

    while (l.pos < l.len) {
        char c     = src[l.pos];
        int  start = l.pos;
        int  line  = l.line;
        int  col   = l.col;

        /* 1. Skip whitespace silently. */
        if (is_whitespace(c)) { advance(&l); continue; }

        /* 2. Identifiers and keywords — starts with a letter. */
        if (is_letter(c)) {
            advance(&l);
            if (lex_ident(&l, start, line, col) < 0) return -1;
            continue;
        }
        /* 3. Numeric literals — starts with a digit. */
        if (is_digit(c)) {
            if (lex_number(&l, start, line, col) < 0) return -1;
            continue;
        }
        /* 4. String literals — starts with '"'. */
        if (c == '"') {
            advance(&l);
            if (lex_string(&l, start, line, col) < 0) return -1;
            continue;
        }

        /* 5. Single-character symbols. */
        TokenKind sym;
        switch (c) {
        case '(':  sym = TK_LPAREN;   break;
        case ')':  sym = TK_RPAREN;   break;
        case '{':  sym = TK_LBRACE;   break;
        case '}':  sym = TK_RBRACE;   break;
        case '[':  sym = TK_LBRACKET; break;
        case ']':  sym = TK_RBRACKET; break;
        case '=':  sym = TK_EQ;       break;
        case ':':  sym = TK_COLON;    break;
        case '.':  sym = TK_DOT;      break;
        case ';':  sym = TK_SEMI;     break;
        case '+':  sym = TK_PLUS;     break;
        case '-':  sym = TK_MINUS;    break;
        case '*':  sym = TK_STAR;     break;
        case '/':  sym = TK_SLASH;    break;
        case '<':  sym = TK_LT;       break;
        case '>':  sym = TK_GT;       break;
        case '!':  sym = TK_BANG;     break;
        case '|':  sym = TK_PIPE;     break;
        default:
            diag_emit(DIAG_ERROR, LEX_E1003, start, line, col,
                      "character outside Profile 1 character set", NULL);
            return -1;
        }
        advance(&l);
        if (emit(&l, sym, start, 1, line, col) < 0) return -1;
    }

    /* Append the EOF sentinel so the parser always has a clean terminator. */
    if (emit(&l, TK_EOF, l.pos, 0, l.line, l.col) < 0) return -1;
    return l.out_n;
}
