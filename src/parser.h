#ifndef TK_PARSER_H
#define TK_PARSER_H

/*
 * parser.h — AST node types, Token/TokenKind definitions, and parse() entry
 * point for the toke reference compiler.
 *
 * TokenKind and Token definitions migrated to lexer.h (story 1.2.1 complete).
 * This file includes lexer.h and adds a TK_SEMICOLON alias for TK_SEMI.
 *
 * Story: 1.2.2  Branch: feature/compiler-parser
 */

#include "arena.h"
#include "diag.h"
#include "lexer.h"  /* TokenKind, Token, lex() — single authoritative definition */

/* TK_SEMICOLON alias for readability in parser code */
#define TK_SEMICOLON TK_SEMI

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
