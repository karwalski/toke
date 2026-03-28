#include <string.h>

#include "diag.h"
#include "lexer.h"

#define LEX_E1001 1001
#define LEX_E1002 1002
#define LEX_E1003 1003
#define LEX_W1010 1010

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

static int  lex_string(Lexer *l, int start, int line, int col);
static int  lex_number(Lexer *l, int start, int line, int col);
static int  lex_ident (Lexer *l, int start, int line, int col);
static void advance(Lexer *l);
static int  emit(Lexer *l, TokenKind k, int start, int len, int line, int col);

static int is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_ident_cont(char c) { return is_letter(c) || is_digit(c); }
static int is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

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
                          "string interpolation \\( is not supported in Phase 1; "
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

static int lex_ident(Lexer *l, int start, int line, int col)
{
    while (l->pos < l->len && is_ident_cont(l->src[l->pos])) advance(l);
    int len = l->pos - start;
    return emit(l, classify_ident(l->src, start, len), start, len, line, col);
}

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

        if (is_whitespace(c)) { advance(&l); continue; }

        if (is_letter(c)) {
            advance(&l);
            if (lex_ident(&l, start, line, col) < 0) return -1;
            continue;
        }
        if (is_digit(c)) {
            if (lex_number(&l, start, line, col) < 0) return -1;
            continue;
        }
        if (c == '"') {
            advance(&l);
            if (lex_string(&l, start, line, col) < 0) return -1;
            continue;
        }

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
                      "character outside Phase 1 character set", NULL);
            return -1;
        }
        advance(&l);
        if (emit(&l, sym, start, 1, line, col) < 0) return -1;
    }

    if (emit(&l, TK_EOF, l.pos, 0, l.line, l.col) < 0) return -1;
    return l.out_n;
}
