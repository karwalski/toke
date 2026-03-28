#ifndef TK_PARSER_H
#define TK_PARSER_H

/*
 * parser.h — AST node types, Token/TokenKind definitions, and parse() entry
 * point for the toke reference compiler.
 *
 * Token and TokenKind are defined here because lexer.h is still a stub;
 * they will migrate to lexer.h in story 1.2.1.
 *
 * Story: 1.2.2  Branch: feature/compiler-parser
 */

#include "arena.h"
#include "diag.h"

/* ────────────────────────────────────────────────────────────────────────── */
/* Token kinds — 38 total: 12 keywords, 4 literals, 2 identifier classes,   */
/* 18 symbols, TK_EOF, TK_ERROR                                              */
/* ────────────────────────────────────────────────────────────────────────── */

typedef enum {
    /* Literals */
    TK_INT_LIT,       /* integer literal (decimal / hex / binary) */
    TK_FLOAT_LIT,     /* floating-point literal                   */
    TK_STR_LIT,       /* string literal — span includes " delims  */
    TK_BOOL_LIT,      /* true | false                             */

    /* Identifiers */
    TK_IDENT,         /* lowercase-initial user identifier        */
    TK_TYPE_IDENT,    /* uppercase-initial identifier             */

    /* Keywords — 12 */
    TK_KW_F,          /* F  — function declaration                */
    TK_KW_T,          /* T  — type declaration                    */
    TK_KW_I,          /* I  — import declaration                  */
    TK_KW_M,          /* M  — module declaration                  */
    TK_KW_IF,         /* if                                       */
    TK_KW_EL,         /* el — else                                */
    TK_KW_LP,         /* lp — loop                                */
    TK_KW_BR,         /* br — break                               */
    TK_KW_LET,        /* let                                      */
    TK_KW_MUT,        /* mut                                      */
    TK_KW_AS,         /* as  — cast                               */
    TK_KW_RT,         /* rt  — return (long form)                 */

    /* Symbols — 18 */
    TK_EQ,            /* =  */
    TK_LT,            /* <  */
    TK_GT,            /* >  */
    TK_PLUS,          /* +  */
    TK_MINUS,         /* -  */
    TK_STAR,          /* *  */
    TK_SLASH,         /* /  */
    TK_BANG,          /* !  */
    TK_PIPE,          /* |  */
    TK_DOT,           /* .  */
    TK_COLON,         /* :  */
    TK_SEMICOLON,     /* ;  */
    TK_LPAREN,        /* (  */
    TK_RPAREN,        /* )  */
    TK_LBRACE,        /* {  */
    TK_RBRACE,        /* }  */
    TK_LBRACKET,      /* [  */
    TK_RBRACKET,      /* ]  */

    /* Sentinels */
    TK_EOF,
    TK_ERROR
} TokenKind;

/* ────────────────────────────────────────────────────────────────────────── */
/* Token — produced by the lexer, consumed by the parser                     */
/* ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    TokenKind kind;
    int       start;  /* byte offset into source buffer */
    int       len;    /* length in bytes                */
    int       line;   /* 1-based line number            */
    int       col;    /* 1-based column number          */
} Token;

/* ────────────────────────────────────────────────────────────────────────── */
/* AST node kinds                                                             */
/* ────────────────────────────────────────────────────────────────────────── */

typedef enum {
    NODE_PROGRAM,
    NODE_MODULE,
    NODE_IMPORT,
    NODE_FUNC_DECL,
    NODE_TYPE_DECL,
    NODE_CONST_DECL,
    NODE_PARAM,
    NODE_FIELD,
    NODE_STMT_LIST,
    NODE_BIND_STMT,
    NODE_MUT_BIND_STMT,
    NODE_ASSIGN_STMT,
    NODE_RETURN_STMT,
    NODE_IF_STMT,
    NODE_LOOP_STMT,
    NODE_BREAK_STMT,
    NODE_ARENA_STMT,
    NODE_MATCH_STMT,
    NODE_EXPR_STMT,
    NODE_BINARY_EXPR,
    NODE_UNARY_EXPR,
    NODE_CALL_EXPR,
    NODE_CAST_EXPR,
    NODE_PROPAGATE_EXPR,
    NODE_INDEX_EXPR,
    NODE_FIELD_EXPR,
    NODE_IDENT,
    NODE_TYPE_IDENT,
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_STR_LIT,
    NODE_BOOL_LIT,
    NODE_ARRAY_LIT,
    NODE_STRUCT_LIT,
    NODE_FIELD_INIT,
    NODE_TYPE_EXPR,
    NODE_ARRAY_TYPE,
    NODE_FUNC_TYPE,
    NODE_MATCH_ARM,
    NODE_RETURN_SPEC,
    NODE_MODULE_PATH,
    NODE_LOOP_INIT
} NodeKind;

/* ────────────────────────────────────────────────────────────────────────── */
/* AST Node — discriminated union, arena-allocated                           */
/* ────────────────────────────────────────────────────────────────────────── */

#define NODE_MAX_CHILDREN 8

typedef struct Node Node;
struct Node {
    NodeKind kind;
    int      start;         /* byte offset of first token       */
    int      line;          /* 1-based line of first token      */
    int      col;           /* 1-based column of first token    */
    Node    *children[NODE_MAX_CHILDREN];
    int      child_count;
    /* Leaf-node token span (meaningful when child_count == 0) */
    int      tok_start;     /* byte offset                      */
    int      tok_len;       /* length in bytes                  */
    /* Operator / keyword stored for binary/unary nodes */
    TokenKind op;
};

/* ────────────────────────────────────────────────────────────────────────── */
/* Diagnostic severity and error codes                                       */
/* ────────────────────────────────────────────────────────────────────────── */

/* These must match the definitions in diag.h when that stub is implemented. */
#ifndef DIAG_SEVERITY_DEFINED
#define DIAG_SEVERITY_DEFINED
typedef enum { DIAG_WARNING = 0, DIAG_ERROR = 1 } DiagSeverity;
#endif

#define E2001 2001   /* declaration ordering violation              */
#define E2002 2002   /* unexpected token (expected X, got Y)        */
#define E2003 2003   /* missing semicolon                           */
#define E2004 2004   /* unclosed delimiter                          */

/*
 * diag_emit — forward declaration matching the variadic convention.
 * Real implementation provided by diag.c (story 1.2.6).
 * Signature: severity, code, byte_offset, line, col, message, then
 * key/value pairs terminated by a NULL key, with "fix" as the last pair.
 */
void diag_emit(DiagSeverity severity, int code,
               int offset, int line, int col,
               const char *message, ...);

/* ────────────────────────────────────────────────────────────────────────── */
/* Arena — forward declaration (stub filled in by story 1.2.x)               */
/* ────────────────────────────────────────────────────────────────────────── */

#ifndef TK_ARENA_TYPES_DEFINED
#define TK_ARENA_TYPES_DEFINED
typedef struct Arena Arena;
void *arena_alloc(Arena *arena, int size);
#endif

/* ────────────────────────────────────────────────────────────────────────── */
/* Parser entry point                                                         */
/* ────────────────────────────────────────────────────────────────────────── */

/*
 * parse — parse a flat token array into an AST.
 *
 * tokens : array of Token produced by the lexer
 * count  : number of tokens (must end with TK_EOF)
 * src    : original source bytes (used for token text comparison)
 * arena  : allocation arena for AST nodes
 *
 * Returns the root NODE_PROGRAM node, or NULL on fatal (unrecoverable) error.
 */
Node *parse(Token *tokens, int count, const char *src, Arena *arena);

#endif /* TK_PARSER_H */
