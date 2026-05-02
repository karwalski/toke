#ifndef TK_LEXER_H
#define TK_LEXER_H

/*
 * lexer.h — token definitions and lex() entry point for the toke reference
 * compiler.
 */

/* Profile selects the character set and keyword recognition rules. */
typedef enum {
    PROFILE_DEFAULT = 0,   /* 56-char syntax: lowercase keywords, $ for types, @ */
    PROFILE_LEGACY  = 1    /* 80-char syntax: uppercase keywords, TK_TYPE_IDENT  */
} Profile;

typedef enum {
    /* Keywords (13) */
    TK_KW_F,
    TK_KW_T,
    TK_KW_I,
    TK_KW_M,
    TK_KW_IF,
    TK_KW_EL,
    TK_KW_LP,
    TK_KW_BR,
    TK_KW_LET,
    TK_KW_MUT,
    TK_KW_AS,
    TK_KW_RT,
    TK_KW_SC,
    TK_KW_MT,

    /* Literals */
    TK_BOOL_LIT,
    TK_INT_LIT,
    TK_FLOAT_LIT,
    TK_STR_LIT,

    /* Identifiers */
    TK_IDENT,
    TK_TYPE_IDENT,

    /* Symbols */
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_EQ,
    TK_COLON,
    TK_DOT,
    TK_SEMI,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_LT,
    TK_GT,
    TK_BANG,
    TK_PIPE,
    TK_DOLLAR,
    TK_AT,
    TK_AND,
    TK_OR,
    TK_AMP,
    TK_CARET,
    TK_TILDE,
    TK_SHR,
    TK_SHL,
    TK_PERCENT,

    /* Special */
    TK_EOF,
    TK_ERROR
} TokenKind;

typedef struct {
    TokenKind kind;
    int       start;
    int       len;
    int       line;
    int       col;
} Token;

int lex(const char *src, int src_len, Token *out, int out_cap, Profile profile);

#endif /* TK_LEXER_H */
