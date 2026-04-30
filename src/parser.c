/*
 * parser.c — LL(1) recursive-descent parser for toke Profile 1.
 *
 * =========================================================================
 * Role in the compiler pipeline
 * =========================================================================
 * The parser is the second stage of compilation.  It receives a flat token
 * array produced by the lexer and builds an arena-allocated AST (Abstract
 * Syntax Tree).  The AST is consumed downstream by the name resolver
 * (names.c), type checker (types.c), and LLVM IR emitter (llvm.c).
 *
 * =========================================================================
 * Grammar overview (toke Profile 1)
 * =========================================================================
 * toke Profile 1 uses keyword prefixes for top-level declarations:
 *   M=  — module declaration
 *   I=  — import declaration
 *   T=  — type (struct) declaration
 *   F=  — function declaration
 *
 * Semicolons (';') serve as separators between statements, parameters, and
 * field initialisers.  The last item before a closing '}' or EOF may omit
 * its trailing semicolon (trailing-semicolon elision — see opt_semi).
 *
 * Return is written `<value` (the '<' token), and loops use the form
 * `lp(init;cond;step){body}`.
 *
 * Source files must follow a strict declaration ordering:
 *   SourceFile = ModuleDecl ImportDecl* TypeDecl* ConstDecl* FuncDecl* EOF
 *
 * =========================================================================
 * Precedence chain (lowest to highest)
 * =========================================================================
 *   CompareExpr  →  AddExpr  →  MulExpr  →  UnaryExpr  →  CastPropExpr
 *   →  CallExpr  →  PostfixExpr  →  PrimaryExpr
 *
 * Match expressions (pipe '|') bind at the top of the expression chain,
 * below comparison level.
 *
 * =========================================================================
 * Error recovery strategy
 * =========================================================================
 * The parser uses a simple panic-mode strategy via sync():
 *   - On any unexpected token, emit a diagnostic (E2002/E2003/E2004).
 *   - Advance tokens until a semicolon or closing brace is found, then
 *     resume parsing at that synchronisation point.
 *   - A hard error cap (errs > 16) prevents runaway diagnostics on badly
 *     malformed input.
 *
 * All AST nodes are arena-allocated — no explicit free is needed.
 *
 * =========================================================================
 * Diagnostic codes emitted by the parser
 * =========================================================================
 *   E2001 — Declaration ordering violation (M before I before T before F).
 *   E2002 — Unexpected token (expected X, got Y).
 *   E2003 — Missing semicolon between statements.
 *   E2004 — Unclosed delimiter (parenthesis, bracket, or brace).
 *   E2005 — Unexpected token in type position.
 *   E2015 — Duplicate field name in struct declaration.
 *
 * Story: 1.2.2  Comments: 10.11.2
 * =========================================================================
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "parser.h"

/*
 * Parser — private recursive-descent state.
 *
 *   toks — pointer to the lexer's token array (not owned)
 *   n    — total number of tokens (including the trailing TK_EOF)
 *   pos  — current read position (index into toks[])
 *   src  — original source text, used by teq() for lexeme comparison
 *   a    — arena allocator for all AST node memory
 *   errs — running count of emitted errors (used for the bail-out cap)
 */
typedef struct { Token *toks; int n; int pos; const char *src; Arena *a; int errs; Profile profile; int in_sc; } Parser;

/*
 * peek — return the TokenKind at the current position without advancing.
 *
 * Returns TK_EOF when the parser has consumed all tokens.  This makes
 * every lookahead check safe without an explicit bounds test.
 */
static TokenKind peek(Parser *p)  { return p->pos < p->n ? p->toks[p->pos].kind : TK_EOF; }

/*
 * cur — return a pointer to the Token at the current position.
 *
 * If pos is at or past the end, returns the last token (which is always
 * the TK_EOF sentinel) so that line/col information remains valid for
 * diagnostics emitted after the end of input.
 */
static Token    *cur(Parser *p)   { return &p->toks[p->pos < p->n ? p->pos : p->n-1]; }

/*
 * adv — return the current Token pointer, then advance pos by one.
 *
 * Stops at n-1 (the TK_EOF sentinel) to prevent overrun.  The returned
 * pointer is typically passed to mk() to anchor an AST node's source
 * location at the token that triggered the production.
 */
static Token    *adv(Parser *p)   { Token *t = cur(p); if (p->pos < p->n-1) p->pos++; return t; }

/*
 * peek_at — return the TokenKind at a given offset from current position.
 */
static TokenKind peek_at(Parser *p, int off) { int i=p->pos+off; return i<p->n ? p->toks[i].kind : TK_EOF; }

/*
 * mk — allocate and initialise a new AST Node.
 *
 * Allocates from the parser's arena.  The node's source location (start,
 * line, col) is copied from the anchor token `t`.  The children array is
 * pre-allocated to NODE_INIT_CAP slots; ch() doubles it when needed.
 *
 * The `op` field is initialised to TK_EOF (a sentinel meaning "no
 * operator"); parse_unary, parse_mul, parse_add, and parse_compare
 * overwrite it with the actual operator token kind.
 *
 * Returns NULL only on arena exhaustion.
 */
static Node *mk(Parser *p, NodeKind k, Token *t) {
    Node *n = (Node *)arena_alloc(p->a, (int)sizeof(Node));
    if (!n) return NULL;
    n->kind=k; n->start=t->start; n->line=t->line; n->col=t->col;
    n->tok_start=t->start; n->tok_len=t->len; n->child_count=0; n->op=TK_EOF;
    n->child_cap=NODE_INIT_CAP;
    n->children=(Node**)arena_alloc(p->a,(int)(NODE_INIT_CAP*sizeof(Node*)));
    return n;
}

/*
 * ch — append a child node to a parent's children array.
 *
 * If the children array is full, doubles its capacity by allocating a
 * new array from the arena and copying the existing pointers.  The old
 * array is abandoned (arena memory is never freed individually).
 *
 * Silently returns if either parent or child is NULL, which allows
 * callers to chain ch(p, n, parse_xxx(p)) without checking every
 * intermediate result.
 */
static void ch(Parser *p, Node *par, Node *c) {
    if (!par||!c) return;
    if (par->child_count>=par->child_cap) {
        int nc=par->child_cap*2;
        Node **nb=(Node**)arena_alloc(p->a,(int)(nc*(int)sizeof(Node*)));
        if (!nb) return;
        memcpy(nb,par->children,(size_t)par->child_count*sizeof(Node*));
        par->children=nb;
        par->child_cap=nc;
    }
    par->children[par->child_count++]=c;
}

/*
 * teq — test whether a token's lexeme equals a given string.
 *
 * Compares byte-by-byte against the source buffer (p->src) using the
 * token's start offset and length.  Used by is_scalar() to recognise
 * built-in type names, and by parse_stmt() to detect the "arena" keyword.
 */
static int  teq(Parser *p, Token *t, const char *s) { int l=(int)strlen(s); return t->len==l&&!memcmp(p->src+t->start,s,(size_t)l); }

/*
 * eerr — emit an error diagnostic and increment the error counter.
 *
 * Wraps diag_emit() with DIAG_ERROR severity.  The "fix" key-value pair
 * is always NULL (no automated fix suggestion from the parser).
 */
static void eerr(Parser *p, int code, Token *t, const char *msg) { p->errs++; diag_emit(DIAG_ERROR,code,t->start,t->line,t->col,msg,"fix",NULL); }

/*
 * ewarn — emit a warning diagnostic (does not increment error counter).
 *
 * Wraps diag_emit() with DIAG_WARNING severity and a caller-supplied fix hint.
 */
static void ewarn(Parser *p, int code, Token *t, const char *msg, const char *fix) {
    (void)p;
    diag_emit(DIAG_WARNING, code, t->start, t->line, t->col, msg, "fix", fix, NULL);
}

/*
 * sync — panic-mode error recovery.
 *
 * Advances tokens until a synchronisation point is reached: either a
 * semicolon (statement boundary) or a closing brace (block boundary).
 * After consuming the synchronisation token the parser resumes normal
 * parsing.  If TK_EOF is reached first, the function returns without
 * consuming it, allowing the caller's loop to terminate naturally.
 */
static void sync(Parser *p) { while(peek(p)!=TK_EOF){TokenKind k=peek(p);adv(p);if(k==TK_SEMICOLON||k==TK_RBRACE)return;} }

/*
 * opt_semi — consume an optional semicolon separator.
 *
 * Implements toke's trailing-semicolon elision rule: the last statement
 * in a block (before '}') or at end-of-file need not carry a trailing
 * semicolon.  If the next token is neither ';' nor '}'/EOF, a missing-
 * semicolon diagnostic (E2003) is emitted.
 */
static void opt_semi(Parser *p) {
    if(peek(p)==TK_SEMICOLON){adv(p);return;}
    if(peek(p)==TK_RBRACE||peek(p)==TK_EOF) return;
    eerr(p,E2003,cur(p),"missing semicolon");
}

/*
 * xp — expect and consume a specific token kind.
 *
 * If the current token matches kind `k`, advances and returns the token
 * pointer.  Otherwise emits E2002 (unexpected token) or E2004 (if at
 * EOF, indicating an unclosed delimiter) and returns NULL.
 *
 * The `w` parameter is a human-readable description of the expected
 * token for potential future diagnostic messages (currently unused after
 * the error message was simplified).
 */
static Token *xp(Parser *p, TokenKind k, const char *w) {
    (void)w; if(peek(p)==k) return adv(p);
    eerr(p, peek(p)==TK_EOF?E2004:E2002, cur(p), "unexpected token"); return NULL;
}

/* Forward declarations — these three functions participate in mutual
 * recursion: parse_expr calls parse_primary which may call parse_expr
 * (parenthesised sub-expressions, struct literal values, array/map
 * elements); parse_stmt_list calls parse_stmt which calls parse_expr;
 * parse_type_expr recurses for pointer/array/map/function types. */
static Node *parse_expr(Parser *p);
static Node *parse_stmt_list(Parser *p, Token *ref);
static Node *parse_type_expr(Parser *p);
static void  parse_one_param(Parser *p, Node *fn);

/* ── Type expressions ─────────────────────────────────────────────── */

/*
 * is_scalar — test whether a token names a built-in scalar type.
 *
 * Recognises the 14 Profile 1 scalar types: u8, u16, u32, u64, i8, i16,
 * i32, i64, f32, f64, bool, Str, Byte, void.  The token must be either
 * TK_IDENT or TK_TYPE_IDENT (because "Str" and "Byte" start with an
 * uppercase letter and are lexed as TK_TYPE_IDENT).
 *
 * Returns 1 if the token is a scalar type name, 0 otherwise.
 */
static int is_scalar(Parser *p, Token *t) {
    if (t->kind!=TK_IDENT&&t->kind!=TK_TYPE_IDENT) return 0;
    static const char *sc[]={"u8","u16","u32","u64","i8","i16","i32","i64","f32","f64","bool","Str","Byte","void",NULL};
    for(int i=0;sc[i];i++) if(teq(p,t,sc[i])) return 1;
    return 0;
}

/*
 * parse_func_type — parse a function type expression.
 *
 * Grammar:
 *   FuncTypeExpr = '(' TypeExpr (';' TypeExpr)* ')' ':' TypeExpr
 *
 * AST node: NODE_FUNC_TYPE
 *   children[0..n-2] — parameter types (each a TypeExpr node)
 *   children[n-1]    — return type (a TypeExpr node)
 *
 * Example:  (i32;i32):bool   — function taking two i32, returning bool.
 *
 * Error recovery: emits E2004 on unclosed ')'; returns partial node.
 */
/* FuncTypeExpr = '(' TypeExpr (';' TypeExpr)* ')' ':' TypeExpr */
static Node *parse_func_type(Parser *p) {
    Token *t=xp(p,TK_LPAREN,"'('"); if(!t) return NULL;
    Node *n=mk(p,NODE_FUNC_TYPE,t);
    if(peek(p)!=TK_RPAREN){ch(p,n,parse_type_expr(p));while(peek(p)==TK_SEMICOLON){adv(p);ch(p,n,parse_type_expr(p));}}
    if(!xp(p,TK_RPAREN,"')'")) eerr(p,E2004,cur(p),"unclosed delimiter");
    if(xp(p,TK_COLON,"':'")) ch(p,n,parse_type_expr(p));
    return n;
}

/*
 * parse_type_expr — parse a type expression (recursive).
 *
 * Grammar:
 *   TypeExpr = '*' TypeExpr           — pointer type
 *            | '[' TypeExpr ']'       — array type
 *            | '[' TypeExpr ':' TypeExpr ']'  — map type
 *            | '(' ... ')' ':' ...    — function type (delegates to parse_func_type)
 *            | ScalarType             — built-in scalar (u8, i32, Str, etc.)
 *            | TYPE_IDENT             — user-defined type name
 *
 * AST nodes produced:
 *   NODE_PTR_TYPE   — children[0] is the pointee TypeExpr
 *   NODE_ARRAY_TYPE — children[0] is the element TypeExpr
 *   NODE_MAP_TYPE   — children[0] is key TypeExpr, children[1] is value TypeExpr
 *   NODE_FUNC_TYPE  — via parse_func_type
 *   NODE_TYPE_EXPR  — leaf node for scalar types (lexeme in tok_start/tok_len)
 *   NODE_TYPE_IDENT — leaf node for user-defined type names
 *
 * The function distinguishes array vs. map by lookahead: after parsing
 * the first type inside '[', if a ':' follows it is a map type; otherwise
 * it is an array type.
 *
 * Error recovery: emits E2002 and returns NULL on unrecognised type token.
 */
/* TypeExpr = '*' TypeExpr | ScalarType | TYPE_IDENT | '[' TypeExpr ']' | FuncTypeExpr
 *          | '$' IDENT (default mode: type reference)
 *          | '@' TypeExpr (default mode: array type)
 *          | '@' '(' TypeExpr ':' TypeExpr ')' (default mode: map type) */
static Node *parse_type_expr(Parser *p) {
    if(peek(p)==TK_STAR){Token *t=adv(p);Node *n=mk(p,NODE_PTR_TYPE,t);ch(p,n,parse_type_expr(p));return n;}
    /* Default mode: $ident is a type reference */
    if(peek(p)==TK_DOLLAR){Token *dt=adv(p);Token *nt=cur(p);
        if(!xp(p,TK_IDENT,"type name after '$'")){eerr(p,E2002,dt,"unexpected token");return NULL;}
        return mk(p,NODE_TYPE_IDENT,nt);}
    /* Default mode: @$type for array type, @($key:$val) for map type */
    if(peek(p)==TK_AT){Token *at=adv(p);
        if(peek(p)==TK_LPAREN){adv(p);Node *n=mk(p,NODE_MAP_TYPE,at);
            ch(p,n,parse_type_expr(p));xp(p,TK_COLON,"':' in map type");
            ch(p,n,parse_type_expr(p));
            if(!xp(p,TK_RPAREN,"')' to close map type"))eerr(p,E2004,cur(p),"unclosed delimiter");
            return n;}
        {Node *n=mk(p,NODE_ARRAY_TYPE,at);ch(p,n,parse_type_expr(p));return n;}}
    if(peek(p)==TK_LBRACKET){Token *t=xp(p,TK_LBRACKET,"'['");Node *first=parse_type_expr(p);
        if(peek(p)==TK_COLON){adv(p);Node *n=mk(p,NODE_MAP_TYPE,t);ch(p,n,first);ch(p,n,parse_type_expr(p));if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;}
        Node *n=mk(p,NODE_ARRAY_TYPE,t);ch(p,n,first);if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;}
    if(peek(p)==TK_LPAREN) return parse_func_type(p);
    Token *t=cur(p);
    if(is_scalar(p,t)){Node *n=mk(p,NODE_TYPE_EXPR,t);adv(p);return n;}
    if(peek(p)==TK_TYPE_IDENT){Node *n=mk(p,NODE_TYPE_IDENT,t);adv(p);return n;}
    eerr(p,E2005,t,"unexpected token in type position"); return NULL;
}

/* ── Literals ─────────────────────────────────────────────────────── */

/*
 * parse_literal — parse a literal value token.
 *
 * Grammar:
 *   Literal = INT_LIT | FLOAT_LIT | STR_LIT | BOOL_LIT
 *
 * AST nodes: NODE_INT_LIT, NODE_FLOAT_LIT, NODE_STR_LIT, NODE_BOOL_LIT.
 * Each is a leaf node whose tok_start/tok_len span the literal's lexeme
 * in the source text.
 *
 * Error recovery: emits E2002 and returns NULL if the current token is
 * not a recognised literal kind.
 */
static Node *parse_literal(Parser *p) {
    Token *t=cur(p); NodeKind k;
    switch(peek(p)){case TK_INT_LIT:k=NODE_INT_LIT;break;case TK_FLOAT_LIT:k=NODE_FLOAT_LIT;break;
    case TK_STR_LIT:k=NODE_STR_LIT;break;case TK_BOOL_LIT:k=NODE_BOOL_LIT;break;
    default:eerr(p,E2002,t,"unexpected token");return NULL;}
    adv(p); return mk(p,k,t);
}

/* ── Primary ──────────────────────────────────────────────────────── */

/*
 * parse_primary — parse a primary expression (highest precedence).
 *
 * Grammar:
 *   PrimaryExpr = IDENT
 *               | TYPE_IDENT ('{' FieldInit (';' FieldInit)* '}')?
 *               | '(' Expr ')'
 *               | '[' Expr (';' Expr)* ']'           — array literal
 *               | '[' Expr ':' Expr (';' ...) ']'    — map literal
 *               | '[]'                                — empty array
 *               | Literal
 *
 * AST nodes produced:
 *   NODE_IDENT      — bare identifier reference
 *   NODE_TYPE_IDENT — standalone type name (without struct literal braces)
 *   NODE_STRUCT_LIT — struct literal: children are NODE_FIELD_INIT nodes,
 *                     each with children[0]=value expr; the field name is
 *                     stored in the FIELD_INIT node's tok_start/tok_len
 *   NODE_ARRAY_LIT  — array literal: children are element expressions
 *   NODE_MAP_LIT    — map literal: children are NODE_MAP_ENTRY nodes,
 *                     each with children[0]=key expr, children[1]=value expr
 *
 * The TYPE_IDENT case uses one-token lookahead on '{' to distinguish a
 * struct literal (TypeName { field: val }) from a bare type reference.
 *
 * The '[' case parses the first element, then checks for ':' to decide
 * between array literal and map literal.
 *
 * Parenthesised expressions '(' Expr ')' are unwrapped — no GROUP node
 * is created; the inner expression is returned directly.
 *
 * Falls through to parse_literal for integer, float, string, and boolean
 * literal tokens.
 *
 * Error recovery: struct literal and map/array literal loops break on
 * unexpected tokens; sync() is called within struct field parsing.
 * Unclosed delimiters emit E2004.
 */
static Node *parse_primary(Parser *p) {
    Token *t=cur(p);
    /* Function reference: &name — unary prefix & followed by ident.
     * Disambiguated from binary & (bitwise AND) by position: at expression
     * start (parse_primary), & is always a function reference.  Binary &
     * is handled in parse_bitand between two expressions. */
    if(peek(p)==TK_AMP&&peek_at(p,1)==TK_IDENT){
        adv(p); /* consume '&' */
        Token *nt=cur(p); adv(p); /* consume function name */
        return mk(p,NODE_FUNC_REF,nt);
    }
    /* Closure expression: fn(params):ReturnSpec{body}
     * Disambiguated from a plain ident by checking that the ident is "fn"
     * and the next token is '('.  Story 76.1.9a. */
    if(peek(p)==TK_IDENT&&peek_at(p,1)==TK_LPAREN&&teq(p,cur(p),"fn")){
        Token *ft=adv(p); /* consume 'fn' */
        Node *n=mk(p,NODE_CLOSURE,ft);
        xp(p,TK_LPAREN,"'('");
        if(peek(p)!=TK_RPAREN){
            parse_one_param(p,n);
            while(peek(p)==TK_SEMICOLON&&p->pos+1<p->n&&p->toks[p->pos+1].kind!=TK_RPAREN){adv(p);parse_one_param(p,n);}
        }
        if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
        /* Optional return spec: ':' TypeExpr ('!' TypeExpr)? */
        if(peek(p)==TK_COLON){
            adv(p);
            Token *rs=cur(p); Node *rspec=mk(p,NODE_RETURN_SPEC,rs);
            ch(p,rspec,parse_type_expr(p));
            if(peek(p)==TK_BANG){adv(p);ch(p,rspec,parse_type_expr(p));}
            ch(p,n,rspec);
        }
        /* Body: '{' StmtList '}' */
        if(!xp(p,TK_LBRACE,"'{' for closure body")){sync(p);return n;}
        ch(p,n,parse_stmt_list(p,ft));
        if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
        return n;
    }
    /* spawn context keyword inside sc block: spawn expr */
    if(p->in_sc&&peek(p)==TK_IDENT&&teq(p,cur(p),"spawn")){
        Token *st=cur(p);adv(p);
        Node *sp=mk(p,NODE_SPAWN_EXPR,st);ch(p,sp,parse_expr(p));return sp;
    }
    if(peek(p)==TK_IDENT){adv(p);return mk(p,NODE_IDENT,t);}
    if(peek(p)==TK_TYPE_IDENT){
        adv(p);
        if(peek(p)==TK_LBRACE){ /* StructLit = TypeName '{' FieldInit (';' FieldInit)* '}' */
            Node *n=mk(p,NODE_STRUCT_LIT,t); xp(p,TK_LBRACE,"'{'");
            while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF){
                Token *ft=cur(p); if(peek(p)==TK_IDENT||peek(p)==TK_TYPE_IDENT){adv(p);}else{xp(p,TK_IDENT,"field name");sync(p);break;}
                Node *fi=mk(p,NODE_FIELD_INIT,ft); if(!xp(p,TK_COLON,"':'")){ sync(p);break;}
                ch(p,fi,parse_expr(p)); ch(p,n,fi); if(peek(p)==TK_SEMICOLON)adv(p);else break;
            }
            if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter"); return n;
        }
        return mk(p,NODE_TYPE_IDENT,t);
    }
    /* Default mode: $name{...} is struct literal, $name alone is type ref */
    if(peek(p)==TK_DOLLAR){
        adv(p); /* consume $ */
        Token *nt=cur(p);
        if(peek(p)!=TK_IDENT){eerr(p,E2002,nt,"expected identifier after '$'");return NULL;}
        adv(p); /* consume ident */
        if(peek(p)==TK_LBRACE){ /* $Name{field:val; ...} */
            Node *n=mk(p,NODE_STRUCT_LIT,nt); xp(p,TK_LBRACE,"'{'");
            while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF){
                Token *ft=cur(p);
                /* Field name: ident, TYPE_IDENT (legacy), or $ident (sum variant) */
                if(peek(p)==TK_DOLLAR){adv(p);ft=cur(p);}
                if(peek(p)==TK_IDENT||peek(p)==TK_TYPE_IDENT){adv(p);}else{xp(p,TK_IDENT,"field name");sync(p);break;}
                Node *fi=mk(p,NODE_FIELD_INIT,ft); if(!xp(p,TK_COLON,"':'")){ sync(p);break;}
                ch(p,fi,parse_expr(p)); ch(p,n,fi); if(peek(p)==TK_SEMICOLON)adv(p);else break;
            }
            if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter"); return n;
        }
        return mk(p,NODE_TYPE_IDENT,nt);
    }
    /* Default mode: @(...) array/map literal */
    if(peek(p)==TK_AT){
        Token *at=adv(p); /* consume @ */
        if(!xp(p,TK_LPAREN,"'(' after '@'")){return NULL;}
        if(peek(p)==TK_RPAREN){adv(p);return mk(p,NODE_ARRAY_LIT,at);} /* empty @() */
        Node *first=parse_expr(p);
        if(peek(p)==TK_COLON){ /* MapLit: @(key:val; ...) */
            adv(p);Node *n=mk(p,NODE_MAP_LIT,at);
            Node *entry=mk(p,NODE_MAP_ENTRY,at);ch(p,entry,first);ch(p,entry,parse_expr(p));ch(p,n,entry);
            while(peek(p)==TK_SEMICOLON){adv(p);if(peek(p)==TK_RPAREN)break;
                Token *et=cur(p);Node *e2=mk(p,NODE_MAP_ENTRY,et);ch(p,e2,parse_expr(p));
                if(!xp(p,TK_COLON,"':'"))break;ch(p,e2,parse_expr(p));ch(p,n,e2);}
            if(!xp(p,TK_RPAREN,"')' to close map literal"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;}
        /* ArrayLit: @(expr; expr; ...) */
        Node *n=mk(p,NODE_ARRAY_LIT,at);ch(p,n,first);
        while(peek(p)==TK_SEMICOLON){adv(p);if(peek(p)==TK_RPAREN)break;ch(p,n,parse_expr(p));}
        if(!xp(p,TK_RPAREN,"')' to close array literal"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;
    }
    if(peek(p)==TK_LPAREN){adv(p);Node *e=parse_expr(p);if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");return e;}
    if(peek(p)==TK_LBRACKET){ /* ArrayLit or MapLit (legacy mode) */
        Token *lt=xp(p,TK_LBRACKET,"'['");
        if(peek(p)==TK_RBRACKET){/* empty array [] */
            Node *n=mk(p,NODE_ARRAY_LIT,lt);adv(p);return n;}
        Node *first=parse_expr(p);
        if(peek(p)==TK_COLON){/* MapLit = '[' Expr ':' Expr (';' Expr ':' Expr)* ']' */
            adv(p);Node *n=mk(p,NODE_MAP_LIT,lt);
            Node *entry=mk(p,NODE_MAP_ENTRY,lt);ch(p,entry,first);ch(p,entry,parse_expr(p));ch(p,n,entry);
            while(peek(p)==TK_SEMICOLON){adv(p);if(peek(p)==TK_RBRACKET)break;
                Token *et=cur(p);Node *e2=mk(p,NODE_MAP_ENTRY,et);ch(p,e2,parse_expr(p));
                if(!xp(p,TK_COLON,"':'"))break;ch(p,e2,parse_expr(p));ch(p,n,e2);}
            if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;}
        /* ArrayLit = '[' Expr (';' Expr)* ']' */
        Node *n=mk(p,NODE_ARRAY_LIT,lt);ch(p,n,first);
        while(peek(p)==TK_SEMICOLON){adv(p);ch(p,n,parse_expr(p));}
        if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter"); return n;
    }
    return parse_literal(p);
}

/* ── Expression precedence chain ──────────────────────────────────── */

/*
 * parse_postfix — parse postfix operations (field access, indexing).
 *
 * Grammar:
 *   PostfixExpr = PrimaryExpr ('.' IDENT | '[' Expr ']')*
 *
 * AST nodes produced:
 *   NODE_FIELD_EXPR — field access (e.g. `point.x`)
 *     children[0] = receiver expression
 *     children[1] = NODE_IDENT for the field name
 *   NODE_INDEX_EXPR — index/subscript (e.g. `arr[i]`)
 *     children[0] = receiver expression
 *     children[1] = index expression
 *
 * Both operators are left-associative and may chain: `a.b[i].c` produces
 * a nested tree of FIELD_EXPR and INDEX_EXPR nodes.
 *
 * Error recovery: breaks the loop on missing field name after '.';
 * emits E2004 on unclosed ']'.
 */
/* PostfixExpr = PrimaryExpr ('.' IDENT | '.' 'get' '(' Expr ')' | '[' Expr ']')* */
static Node *parse_postfix(Parser *p) {
    Node *l=parse_primary(p);
    for(;;){
        if(peek(p)==TK_DOT){
            Token *d=adv(p);Token *f=cur(p);
            /* .get(expr) → NODE_INDEX_EXPR (array/map indexing in default mode)
             * Disambiguate: only treat as index when the arg list is a single
             * expression (no ';' before ')').  Multi-arg .get(...;...) is a
             * normal method call — fall through to field-access so parse_call
             * picks up the '(' later. */
            if(peek(p)==TK_IDENT&&teq(p,f,"get")&&peek_at(p,1)==TK_LPAREN){
                int save=p->pos;
                adv(p); /* consume "get" */
                adv(p); /* consume ( */
                Node *idx=parse_expr(p);
                if(peek(p)==TK_RPAREN){
                    /* Single-arg .get(expr) — index expression */
                    adv(p); /* consume ) */
                    Node *n=mk(p,NODE_INDEX_EXPR,d);ch(p,n,l);ch(p,n,idx);
                    l=n;
                } else {
                    /* Multi-arg: rewind and treat as field access + call */
                    p->pos=save;
                    if(!xp(p,TK_IDENT,"field"))break;
                    Node *n=mk(p,NODE_FIELD_EXPR,d);ch(p,n,l);ch(p,n,mk(p,NODE_IDENT,f));l=n;
                }
            } else {
                if(!xp(p,TK_IDENT,"field"))break;
                Node *n=mk(p,NODE_FIELD_EXPR,d);ch(p,n,l);ch(p,n,mk(p,NODE_IDENT,f));l=n;
            }
        }
        else if(peek(p)==TK_LBRACKET){Token *d=adv(p);Node *n=mk(p,NODE_INDEX_EXPR,d);ch(p,n,l);ch(p,n,parse_expr(p));if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter");l=n;}
        else break;
    }
    return l;
}

/*
 * parse_call — parse function call expressions.
 *
 * Grammar:
 *   CallExpr = PostfixExpr ('(' ArgList ')')*
 *   ArgList  = Expr (';' Expr)*
 *
 * AST node: NODE_CALL_EXPR
 *   children[0]   = callee expression (the function being called)
 *   children[1..] = argument expressions
 *
 * Arguments are separated by semicolons (toke's universal separator).
 * Multiple consecutive calls are supported: `f(x)(y)` chains two
 * NODE_CALL_EXPR nodes where the second's callee is the first call.
 *
 * Error recovery: emits E2004 on unclosed ')'.
 */
/* CallExpr = PostfixExpr ('(' ArgList ')')* */
static Node *parse_call(Parser *p) {
    Node *l=parse_postfix(p);
    while(peek(p)==TK_LPAREN){Token *op=adv(p);Node *c=mk(p,NODE_CALL_EXPR,op);ch(p,c,l);
        if(peek(p)!=TK_RPAREN){ch(p,c,parse_expr(p));while(peek(p)==TK_SEMICOLON){adv(p);ch(p,c,parse_expr(p));}}
        if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");l=c;}
    return l;
}

/*
 * parse_cast_prop — parse type cast and error propagation expressions.
 *
 * Grammar:
 *   CastExpr      = CallExpr ('as' TypeExpr)?
 *   PropagateExpr = CastExpr ('!' TypeExpr)?
 *
 * AST nodes produced:
 *   NODE_CAST_EXPR — type cast (e.g. `x as i64`)
 *     children[0] = source expression
 *     children[1] = target TypeExpr
 *   NODE_PROPAGATE_EXPR — error propagation (e.g. `result!ErrorType`)
 *     children[0] = source expression
 *     children[1] = error TypeExpr to propagate
 *
 * Both operators are non-associative (at most one of each may appear).
 * Cast is checked first, then propagation; both may apply to the same
 * expression: `x as i32 ! MyError`.
 */
/* CastExpr = CallExpr ('as' TypeExpr)?  /  PropagateExpr = CastExpr ('!' TypeExpr)? */
static Node *parse_cast_prop(Parser *p) {
    Node *l=parse_call(p);
    if(peek(p)==TK_KW_AS){Token *t=adv(p);Node *n=mk(p,NODE_CAST_EXPR,t);ch(p,n,l);ch(p,n,parse_type_expr(p));l=n;}
    if(peek(p)==TK_BANG){Token *t=adv(p);Node *n=mk(p,NODE_PROPAGATE_EXPR,t);ch(p,n,l);ch(p,n,parse_type_expr(p));l=n;}
    return l;
}

/*
 * parse_unary — parse prefix unary operators.
 *
 * Grammar:
 *   UnaryExpr = ('-' | '!') UnaryExpr | CastPropExpr
 *
 * AST node: NODE_UNARY_EXPR
 *   op          = TK_MINUS (negation) or TK_BANG (logical not)
 *   children[0] = operand expression
 *
 * Right-recursive, so multiple prefix operators nest: `--x` produces
 * UNARY(MINUS, UNARY(MINUS, x)).
 *
 * Note: '!' at this level is the logical-not prefix operator; the '!'
 * in error propagation (postfix) is handled at a lower precedence level
 * in parse_cast_prop, so there is no ambiguity.
 */
/* UnaryExpr = ('-'|'!'|'~') UnaryExpr | PropagateExpr */
static Node *parse_unary(Parser *p) {
    if(peek(p)==TK_MINUS||peek(p)==TK_BANG||peek(p)==TK_TILDE){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_UNARY_EXPR,t);n->op=op;ch(p,n,parse_unary(p));return n;}
    return parse_cast_prop(p);
}

/*
 * parse_mul — parse multiplicative binary operators.
 *
 * Grammar:
 *   MulExpr = UnaryExpr (('*' | '/') UnaryExpr)*
 *
 * AST node: NODE_BINARY_EXPR
 *   op          = TK_STAR (multiply) or TK_SLASH (divide)
 *   children[0] = left operand
 *   children[1] = right operand
 *
 * Left-associative: `a * b / c` produces BINARY(/, BINARY(*, a, b), c).
 */
/* MulExpr = UnaryExpr (('*'|'/'|'%') UnaryExpr)* */
static Node *parse_mul(Parser *p) {
    Node *l=parse_unary(p);
    while(peek(p)==TK_STAR||peek(p)==TK_SLASH||peek(p)==TK_PERCENT){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_unary(p));l=n;}
    return l;
}

/*
 * parse_add — parse additive binary operators.
 *
 * Grammar:
 *   AddExpr = MulExpr (('+' | '-') MulExpr)*
 *
 * AST node: NODE_BINARY_EXPR
 *   op          = TK_PLUS (add) or TK_MINUS (subtract)
 *   children[0] = left operand
 *   children[1] = right operand
 *
 * Left-associative: `a + b - c` produces BINARY(-, BINARY(+, a, b), c).
 */
/* AddExpr = MulExpr (('+'|'-') MulExpr)* */
static Node *parse_add(Parser *p) {
    Node *l=parse_mul(p);
    while(peek(p)==TK_PLUS||peek(p)==TK_MINUS){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_mul(p));l=n;}
    return l;
}

/*
 * parse_compare — parse comparison operators.
 *
 * Grammar:
 *   CompareExpr = AddExpr (('<' | '>' | '=') AddExpr)?
 *
 * AST node: NODE_BINARY_EXPR
 *   op          = TK_LT (less than), TK_GT (greater than), or TK_EQ (equal)
 *   children[0] = left operand
 *   children[1] = right operand
 *
 * Non-associative: at most one comparison operator per expression.
 * Chained comparisons like `a < b < c` are not valid toke syntax.
 *
 * Note: '=' here is the equality comparison operator in expression
 * context, not the assignment operator (which is handled in parse_stmt).
 */
/* ShiftExpr = AddExpr (('<<'|'>>') AddExpr)* */
static Node *parse_shift(Parser *p) {
    Node *l=parse_add(p);
    while(peek(p)==TK_SHL||peek(p)==TK_SHR){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_add(p));l=n;}
    return l;
}

/* CompareExpr = ShiftExpr (('<'|'>'|'=') ShiftExpr)? */
static Node *parse_compare(Parser *p) {
    Node *l=parse_shift(p);
    if(peek(p)==TK_LT||peek(p)==TK_GT||peek(p)==TK_EQ){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_shift(p));return n;}
    return l;
}

/* BitAndExpr = CompareExpr ('&' CompareExpr)* */
static Node *parse_bitand(Parser *p) {
    Node *l=parse_compare(p);
    while(peek(p)==TK_AMP){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_compare(p));l=n;}
    return l;
}

/* BitXorExpr = BitAndExpr ('^' BitAndExpr)* */
static Node *parse_bitxor(Parser *p) {
    Node *l=parse_bitand(p);
    while(peek(p)==TK_CARET){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_bitand(p));l=n;}
    return l;
}

/* BitOrExpr = BitXorExpr ('|' BitXorExpr)* — note: '|' followed by '{' is match, not bitwise */
static Node *parse_bitor(Parser *p) {
    Node *l=parse_bitxor(p);
    while(peek(p)==TK_PIPE&&peek_at(p,1)!=TK_LBRACE){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_bitxor(p));l=n;}
    return l;
}

/* AndExpr = BitOrExpr ('&&' BitOrExpr)* */
static Node *parse_and(Parser *p) {
    Node *l=parse_bitor(p);
    while(peek(p)==TK_AND){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_bitor(p));l=n;}
    return l;
}

/* OrExpr = AndExpr ('||' AndExpr)* */
static Node *parse_or(Parser *p) {
    Node *l=parse_and(p);
    while(peek(p)==TK_OR){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(p,n,l);ch(p,n,parse_and(p));l=n;}
    return l;
}

/*
 * parse_match_arm — parse a single arm of a match expression.
 *
 * Grammar:
 *   MatchArm = TYPE_IDENT ':' IDENT Expr
 *
 * AST node: NODE_MATCH_ARM
 *   children[0] = NODE_TYPE_IDENT — the pattern type to match against
 *   children[1] = NODE_IDENT      — the binding name for the matched value
 *   children[2] = expression      — the arm's body expression
 *
 * Example:  `Ok: val val + 1`  matches the Ok variant, binds it to `val`,
 * and evaluates `val + 1`.
 *
 * Error recovery: calls sync() on missing ':' or binding name, returning
 * a partial NODE_MATCH_ARM.
 */
/* MatchArm = TypeExpr ':' IDENT Expr
 * Legacy: TYPE_IDENT ':' IDENT Expr   (e.g. Ok:v ...)
 * Default: '$' IDENT ':' IDENT Expr   (e.g. $ok:v ...) */
static Node *parse_match_arm(Parser *p) {
    Token *pt=cur(p);
    if(peek(p)==TK_DOLLAR){
        /* Default mode: $variant match arm head */
        adv(p); /* consume $ */
        Token *nt=cur(p);
        if(!xp(p,TK_IDENT,"variant name after '$'")) return NULL;
        Node *arm=mk(p,NODE_MATCH_ARM,pt); ch(p,arm,mk(p,NODE_TYPE_IDENT,nt));
        if(!xp(p,TK_COLON,"':'")){ sync(p);return arm;}
        Token *bt=cur(p); if(!xp(p,TK_IDENT,"binding name")){sync(p);return arm;}
        ch(p,arm,mk(p,NODE_IDENT,bt)); ch(p,arm,parse_expr(p)); return arm;
    }
    /* Accept both TK_TYPE_IDENT (legacy Ok/Err) and TK_IDENT (default mode
     * where uppercase-initial tokens are lexed as plain idents) */
    if(peek(p)!=TK_TYPE_IDENT&&peek(p)!=TK_IDENT){xp(p,TK_TYPE_IDENT,"match pattern");return NULL;}
    adv(p);
    Node *arm=mk(p,NODE_MATCH_ARM,pt); ch(p,arm,mk(p,NODE_TYPE_IDENT,pt));
    if(!xp(p,TK_COLON,"':'")){ sync(p);return arm;}
    Token *bt=cur(p); if(!xp(p,TK_IDENT,"binding name")){sync(p);return arm;}
    ch(p,arm,mk(p,NODE_IDENT,bt)); ch(p,arm,parse_expr(p)); return arm;
}

/*
 * parse_expr — parse a full expression (top of the precedence chain).
 *
 * Grammar:
 *   Expr = CompareExpr ('|' '{' MatchArm (';' MatchArm)* '}')?
 *
 * If no pipe '|' follows the comparison expression, returns the
 * comparison result directly.  Otherwise, constructs a match expression.
 *
 * AST node: NODE_MATCH_STMT (despite the name, used for match expressions)
 *   children[0]   = scrutinee expression (the value being matched)
 *   children[1..] = NODE_MATCH_ARM nodes
 *
 * Arms are separated by semicolons.  A trailing semicolon before '}'
 * is allowed (the loop checks for '}' after consuming ';').
 *
 * Error recovery: calls sync() on missing '{' after '|'; emits E2004
 * on unclosed '}'.
 */
static Node *parse_expr(Parser *p) {
    Node *l=parse_or(p);
    if(peek(p)!=TK_PIPE) return l;
    Token *t=adv(p); Node *n=mk(p,NODE_MATCH_STMT,t); ch(p,n,l);
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(p,n,parse_match_arm(p));
    while(peek(p)==TK_SEMICOLON){adv(p);if(peek(p)==TK_RBRACE)break;ch(p,n,parse_match_arm(p));}
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    return n;
}

/* ── Statements ───────────────────────────────────────────────────── */

/*
 * parse_loop_stmt — parse a loop statement.
 *
 * Grammar:
 *   LoopStmt = 'lp' '(' LoopInit ';' Expr ';' LoopStep ')' '{' StmtList '}'
 *   LoopInit = ('let' IDENT | IDENT) '=' Expr
 *   LoopStep = IDENT '=' Expr
 *
 * AST node: NODE_LOOP_STMT
 *   children[0] = NODE_LOOP_INIT — initialiser
 *     op = TK_KW_LET if the loop variable is declared with 'let'
 *     children[0] = NODE_IDENT (loop variable name)
 *     children[1] = initialiser expression
 *   children[1] = condition expression (evaluated before each iteration)
 *   children[2] = NODE_ASSIGN_STMT — step/update
 *     children[0] = NODE_IDENT (variable to update)
 *     children[1] = step expression
 *   children[3] = NODE_STMT_LIST — loop body
 *
 * Example:
 *   lp(let i = 0; i < 10; i = i + 1) {
 *       print(i)
 *   }
 *
 * An optional trailing semicolon after the closing '}' is consumed.
 *
 * Error recovery: calls sync() at each structural point (missing '(',
 * '=', ';', identifier); emits E2004 on unclosed ')' or '}'.
 */
/* LoopStmt = 'lp' '(' LoopInit ';' Expr ';' LoopStep ')' '{' StmtList '}' */
static Node *parse_loop_stmt(Parser *p) {
    Token *t=xp(p,TK_KW_LP,"'lp'"); if(!t) return NULL;
    Node *n=mk(p,NODE_LOOP_STMT,t);
    if(!xp(p,TK_LPAREN,"'('")){ sync(p);return n;}
    /* LoopInit = ('let' IDENT | IDENT) '=' Expr */
    Token *it=cur(p); Node *ini=mk(p,NODE_LOOP_INIT,it);
    if(peek(p)==TK_KW_LET){ini->op=TK_KW_LET;adv(p);}
    Token *nt=cur(p); if(xp(p,TK_IDENT,"identifier")){ch(p,ini,mk(p,NODE_IDENT,nt));}
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(p,ini,parse_expr(p)); ch(p,n,ini);
    if(!xp(p,TK_SEMICOLON,"';'")){ sync(p);return n;}
    ch(p,n,parse_expr(p));
    if(!xp(p,TK_SEMICOLON,"';'")){ sync(p);return n;}
    /* LoopStep = IDENT '=' Expr */
    Token *st=cur(p); if(!xp(p,TK_IDENT,"identifier")){ sync(p);return n;}
    Node *step=mk(p,NODE_ASSIGN_STMT,st); ch(p,step,mk(p,NODE_IDENT,st));
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(p,step,parse_expr(p)); ch(p,n,step);
    if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(p,n,parse_stmt_list(p,t));
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(peek(p)==TK_SEMICOLON) adv(p);  /* optional trailing ';' after loop block */
    return n;
}

/*
 * parse_if_stmt — parse an if/else statement.
 *
 * Grammar:
 *   IfStmt = 'if' '(' Expr ')' '{' StmtList '}' ('el' '{' StmtList '}')?
 *
 * AST node: NODE_IF_STMT
 *   children[0] = condition expression
 *   children[1] = NODE_STMT_LIST — then-branch body
 *   children[2] = NODE_STMT_LIST — else-branch body (only present when
 *                 'el' keyword follows the then-block)
 *
 * The else branch is optional.  There is no `else if` syntax in toke
 * Profile 1; nested conditionals require explicit `el { if(...){...} }`.
 *
 * An optional trailing semicolon after the closing '}' is consumed.
 *
 * Error recovery: calls sync() on missing '(' or '{'; emits E2004 on
 * unclosed ')' or '}'.
 */
/* IfStmt = 'if' '(' Expr ')' '{' StmtList '}' ('el' '{' StmtList '}')? */
static Node *parse_if_stmt(Parser *p) {
    Token *t=xp(p,TK_KW_IF,"'if'"); if(!t) return NULL;
    Node *n=mk(p,NODE_IF_STMT,t);
    if(!xp(p,TK_LPAREN,"'('")){ sync(p);return n;}
    ch(p,n,parse_expr(p));
    if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(p,n,parse_stmt_list(p,t));
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(peek(p)==TK_KW_EL){adv(p);
        if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
        ch(p,n,parse_stmt_list(p,t));
        if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");}
    if(peek(p)==TK_SEMICOLON) adv(p);  /* optional trailing ';' after if/el block */
    return n;
}

/*
 * parse_stmt — parse a single statement (dispatches by lookahead).
 *
 * Grammar:
 *   Stmt = BindStmt | MutBindStmt | BreakStmt | ReturnStmt
 *        | IfStmt | LoopStmt | ArenaStmt | AssignStmt | ExprStmt
 *
 * Dispatch rules (by leading token):
 *   TK_KW_LET  → BindStmt:     `let name = expr;`
 *                 MutBindStmt:  `let name = mut. expr;`
 *   TK_KW_BR   → BreakStmt:    `br;`
 *   TK_KW_RT   → ReturnStmt:   `rt expr;`
 *   TK_LT      → ReturnStmt:   `<expr;`  (Profile 1 return syntax)
 *   TK_KW_IF   → delegates to parse_if_stmt
 *   TK_KW_LP   → delegates to parse_loop_stmt
 *   TK_LBRACE  → ArenaStmt:    `{arena ...}`  (one-token lookahead for "arena")
 *   TK_IDENT   → AssignStmt:   `name = expr;` (one-token lookahead for '=')
 *                 ExprStmt:     bare expression (fallthrough when no '=' follows)
 *   default    → ExprStmt:     wrap an expression in NODE_EXPR_STMT
 *
 * AST nodes produced:
 *   NODE_BIND_STMT     — immutable binding: children[0]=ident, children[1]=value
 *   NODE_MUT_BIND_STMT — mutable binding: children[0]=ident, children[1]=value
 *                         (the `mut.` prefix is consumed; the '.' is required)
 *   NODE_BREAK_STMT    — loop break (leaf node, no children)
 *   NODE_RETURN_STMT   — return: children[0]=value expr (optional; a bare
 *                         `<` or `rt` with no expression returns void)
 *   NODE_ARENA_STMT    — arena block: children[0]=NODE_STMT_LIST
 *   NODE_ASSIGN_STMT   — assignment: children[0]=ident, children[1]=value
 *   NODE_EXPR_STMT     — expression statement: children[0]=expression
 *
 * Special case: `let true = ...` or `let false = ...` is rejected with
 * diagnostic 1010 (reserved literal used as identifier).
 *
 * Error recovery: each case calls sync() on structural errors and
 * returns NULL or a partial node.  opt_semi() handles semicolons.
 */
/*
 * check_stmt_keyword_prefix — emit W2020 if a TK_IDENT at statement level
 * starts with a multi-char keyword prefix (let, if, el, lp, br, mut, rt, as)
 * followed by a lowercase letter.  Single-char keywords (m, f, t, i) are
 * excluded to avoid false positives.
 */
static const char *const STMT_KW_PREFIXES[] = {
    "let", "if", "el", "lp", "br", "mut", "rt", "as"
};
#define STMT_KW_PREFIX_COUNT ((int)(sizeof(STMT_KW_PREFIXES) / sizeof(STMT_KW_PREFIXES[0])))

static void check_stmt_keyword_prefix(Parser *p, Token *t) {
    const char *text = p->src + t->start;
    int len = t->len;
    for (int i = 0; i < STMT_KW_PREFIX_COUNT; i++) {
        int kwlen = (int)strlen(STMT_KW_PREFIXES[i]);
        if (len > kwlen &&
            memcmp(text, STMT_KW_PREFIXES[i], (size_t)kwlen) == 0 &&
            text[kwlen] >= 'a' && text[kwlen] <= 'z') {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "identifier '%.*s' starts with keyword '%s' "
                     "-- did you mean '%s %.*s'?",
                     len, text, STMT_KW_PREFIXES[i],
                     STMT_KW_PREFIXES[i], len - kwlen, text + kwlen);
            ewarn(p, W2020, t, msg, "insert a space after the keyword");
            return;
        }
    }
}

/* parse_stmt — dispatches by lookahead */
static Node *parse_stmt(Parser *p) {
    Token *t=cur(p);
    switch(peek(p)){
    case TK_KW_LET:{
        adv(p); Token *nt=cur(p);
        if(peek(p)==TK_BOOL_LIT){
            eerr(p,1010,nt,"reserved literal cannot be used as an identifier");
            sync(p);return NULL;}
        if(!xp(p,TK_IDENT,"identifier")){ sync(p);return NULL;}
        /* Optional type annotation: let x:type = expr */
        Node *type_ann=NULL;
        if(peek(p)==TK_COLON){adv(p);type_ann=parse_type_expr(p);}
        if(!xp(p,TK_EQ,"'='")){ sync(p);return NULL;}
        int mut=(peek(p)==TK_KW_MUT);
        Node *n=mk(p,mut?NODE_MUT_BIND_STMT:NODE_BIND_STMT,t); ch(p,n,mk(p,NODE_IDENT,nt));
        if(type_ann) ch(p,n,type_ann);
        if(mut){adv(p);if(!xp(p,TK_DOT,"'.'")){ sync(p);return n;}}
        /* spawn context keyword inside sc block: let x = spawn expr */
        if(p->in_sc&&peek(p)==TK_IDENT&&teq(p,cur(p),"spawn")){
            Token *st=cur(p);adv(p);
            Node *sp=mk(p,NODE_SPAWN_EXPR,st);ch(p,sp,parse_expr(p));
            ch(p,n,sp);
        } else {
            ch(p,n,parse_expr(p));
        }
        opt_semi(p);
        return n;}
    case TK_KW_BR:{adv(p);Node *n=mk(p,NODE_BREAK_STMT,t);opt_semi(p);return n;}
    case TK_KW_RT:
    case TK_LT:{adv(p);Node *n=mk(p,NODE_RETURN_STMT,t);
        if(peek(p)!=TK_SEMICOLON&&peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF)
            ch(p,n,parse_expr(p));
        opt_semi(p);return n;}
    case TK_KW_SC:{
        /* ScopeStmt: 'sc' '{' StmtList '}' — structured concurrency block */
        adv(p);
        if(!xp(p,TK_LBRACE,"'{'")){sync(p);return NULL;}
        Node *n=mk(p,NODE_SCOPE_STMT,t);
        int prev_sc=p->in_sc; p->in_sc=1;
        ch(p,n,parse_stmt_list(p,t));
        p->in_sc=prev_sc;
        if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
        opt_semi(p);
        return n;}
    case TK_KW_IF: return parse_if_stmt(p);
    case TK_KW_LP: return parse_loop_stmt(p);
    case TK_LBRACE:
        /* ArenaStmt: '{' IDENT('arena') StmtList '}' — lookahead into block */
        if(p->pos+1<p->n&&p->toks[p->pos+1].kind==TK_IDENT&&teq(p,&p->toks[p->pos+1],"arena")){
            adv(p);adv(p);Node *n=mk(p,NODE_ARENA_STMT,t);
            ch(p,n,parse_stmt_list(p,t));
            if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
            return n;}
        eerr(p,E2002,t,"unexpected token");sync(p);return NULL;
    case TK_IDENT:
        check_stmt_keyword_prefix(p, t);
        /* AssignStmt: IDENT '=' Expr ';' — one-token ahead check on '=' */
        if(p->pos+1<p->n&&p->toks[p->pos+1].kind==TK_EQ){
            adv(p);adv(p);Node *n=mk(p,NODE_ASSIGN_STMT,t);ch(p,n,mk(p,NODE_IDENT,t));
            ch(p,n,parse_expr(p));opt_semi(p);return n;}
        __attribute__((fallthrough));
    default:{Node *n=mk(p,NODE_EXPR_STMT,t);ch(p,n,parse_expr(p));opt_semi(p);return n;}
    }
}

/*
 * parse_stmt_list — parse a sequence of statements until '}' or EOF.
 *
 * Grammar:
 *   StmtList = Stmt*
 *
 * AST node: NODE_STMT_LIST
 *   children[0..n] = statement nodes (any of the Stmt variants)
 *
 * The `ref` parameter provides source location for the NODE_STMT_LIST
 * node itself (typically the opening '{' or function keyword token).
 *
 * Infinite-loop protection: if parse_stmt returns without advancing the
 * position (pos unchanged), sync() is called to force progress and
 * prevent hanging on malformed input.
 */
static Node *parse_stmt_list(Parser *p, Token *ref) {
    Node *n=mk(p,NODE_STMT_LIST,ref);
    while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF){int sv=p->pos;Node *s=parse_stmt(p);if(s)ch(p,n,s);if(p->pos==sv){sync(p);}}
    return n;
}

/* ── Top-level declarations ───────────────────────────────────────── */

/*
 * parse_module_path — parse a dot-separated module path.
 *
 * Grammar:
 *   ModulePath = IDENT ('.' IDENT)*
 *
 * AST node: NODE_MODULE_PATH
 *   children[0..n] = NODE_IDENT for each path segment
 *
 * Example: `core.io` produces a MODULE_PATH with two IDENT children
 * ("core" and "io").
 *
 * Error recovery: breaks the loop on missing identifier after '.'.
 */
/* ModulePath = IDENT ('.' IDENT)* */
/* Accept TK_IDENT or TK_TYPE_IDENT as a module path segment — the latter
 * allows uppercase module names like std.Http; names.c normalises them. */
static Token *xp_seg(Parser *p) {
    if (peek(p)==TK_IDENT || peek(p)==TK_TYPE_IDENT) return adv(p);
    eerr(p, peek(p)==TK_EOF?E2004:E2002, cur(p), "unexpected token"); return NULL;
}
static Node *parse_module_path(Parser *p) {
    Token *t=cur(p); if(!xp_seg(p)) return NULL;
    Node *n=mk(p,NODE_MODULE_PATH,t); ch(p,n,mk(p,NODE_IDENT,t));
    while(peek(p)==TK_DOT){adv(p);Token *s=cur(p);if(!xp_seg(p))break;ch(p,n,mk(p,NODE_IDENT,s));}
    return n;
}

/*
 * parse_module_decl — parse a module declaration.
 *
 * Grammar:
 *   ModuleDecl = 'M' '=' ModulePath ';'
 *
 * AST node: NODE_MODULE
 *   children[0] = NODE_MODULE_PATH
 *
 * Example: `M=myapp.utils;`
 *
 * Every toke source file must begin with exactly one module declaration.
 * The parser enforces this in the main parse() function.
 *
 * Error recovery: calls sync() on missing '='.
 */
/* ModuleDecl = 'M' '=' ModulePath ';' */
static Node *parse_module_decl(Parser *p) {
    Token *t=xp(p,TK_KW_M,"'M'"); if(!t) return NULL;
    Node *n=mk(p,NODE_MODULE,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(p,n,parse_module_path(p));
    opt_semi(p);
    return n;
}

/*
 * parse_import_decl — parse an import declaration.
 *
 * Grammar:
 *   ImportDecl = 'I' '=' IDENT ':' ModulePath (STR_LIT)? ';'
 *
 * AST node: NODE_IMPORT
 *   children[0] = NODE_IDENT      — local alias for the imported module
 *   children[1] = NODE_MODULE_PATH — the module being imported
 *   children[2] = NODE_STR_LIT    — optional version constraint string
 *
 * Example: `I=io:core.io "1.0";`  imports core.io as `io` with version "1.0".
 *
 * The version string is optional; if present it is attached as a third
 * child.  The name resolver uses the alias (children[0]) to create a
 * scope binding.
 *
 * Error recovery: calls sync() at each structural point (missing '=',
 * alias, ':').
 */
/* ImportDecl = 'I' '=' IDENT ':' ModulePath (STR_LIT)? ';' */
static Node *parse_import_decl(Parser *p) {
    Token *t=xp(p,TK_KW_I,"'I'"); if(!t) return NULL;
    Node *n=mk(p,NODE_IMPORT,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    Token *at=cur(p); if(!xp(p,TK_IDENT,"import alias")){ sync(p);return n;}
    ch(p,n,mk(p,NODE_IDENT,at));
    if(!xp(p,TK_COLON,"':'")){ sync(p);return n;}
    ch(p,n,parse_module_path(p));
    /* Optional version string: I=alias:module.path "1.2"; */
    if(peek(p)==TK_STR_LIT){
        Token *vt=cur(p); adv(p);
        ch(p,n,mk(p,NODE_STR_LIT,vt));
    }
    opt_semi(p);
    return n;
}

/*
 * parse_field_list — parse a semicolon-separated list of struct fields.
 *
 * Grammar:
 *   FieldList = Field (';' Field)*
 *   Field     = IDENT ':' TypeExpr
 *
 * AST node: NODE_STMT_LIST (reused as a generic list container)
 *   children[0..n] = NODE_FIELD nodes, each with:
 *     tok_start/tok_len = field name lexeme
 *     children[0]       = TypeExpr for the field's type
 *
 * Used exclusively within parse_type_decl to parse struct body fields.
 * Field names may be TK_IDENT, TK_TYPE_IDENT (legacy uppercase variants),
 * or TK_DOLLAR + TK_IDENT (default mode $variant fields in sum types).
 *
 * Semicolons separate fields; a trailing semicolon before '}' is allowed
 * (the loop peeks ahead to avoid consuming a ';' that precedes '}').
 *
 * Error recovery: emits E2002 on unexpected token and breaks; calls
 * sync() on missing ':'.
 */
/* FieldList = Field (';' Field)*  (used by TypeDecl) */
static Node *parse_field_list(Parser *p) {
    Node *n=mk(p,NODE_STMT_LIST,cur(p));
    /* Track field names for duplicate detection (E2015). */
    int seen_starts[64]; int seen_lens[64]; int seen_count=0;
    do {
        Token *ft=cur(p); TokenKind fk=peek(p);
        /* Default mode: $variant field name in sum type (e.g. $notfound:u64) */
        if(fk==TK_DOLLAR){adv(p);ft=cur(p);fk=peek(p);}
        if(fk!=TK_IDENT&&fk!=TK_TYPE_IDENT){ eerr(p,E2002,ft,"unexpected token");break;}
        /* Check for duplicate field name */
        for(int i=0;i<seen_count;i++){
            if(seen_lens[i]==ft->len&&memcmp(p->src+seen_starts[i],p->src+ft->start,(size_t)ft->len)==0){
                eerr(p,E2015,ft,"duplicate field name in struct declaration");
                break;
            }
        }
        if(seen_count<64){seen_starts[seen_count]=ft->start;seen_lens[seen_count]=ft->len;seen_count++;}
        adv(p); Node *f=mk(p,NODE_FIELD,ft);
        if(!xp(p,TK_COLON,"':'")){ sync(p);break;}
        ch(p,f,parse_type_expr(p)); ch(p,n,f);
        if(peek(p)!=TK_SEMICOLON) break;
        if(p->pos+1<p->n&&p->toks[p->pos+1].kind==TK_RBRACE) break;
        adv(p);
    } while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF);
    return n;
}

/*
 * parse_type_decl — parse a type (struct) declaration.
 *
 * Grammar:
 *   TypeDecl = 'T' '=' TYPE_IDENT '{' FieldList '}' ';'
 *
 * AST node: NODE_TYPE_DECL
 *   children[0] = NODE_TYPE_IDENT — the declared type name
 *   children[1] = NODE_STMT_LIST  — field list (from parse_field_list)
 *
 * Example:
 *   T=Point {
 *       x: f64;
 *       y: f64
 *   };
 *
 * Error recovery: calls sync() on missing '=' or type name; emits
 * E2004 on unclosed '}'.
 */
/* TypeDecl = 'T' '=' TypeName '{' FieldList '}' ';' */
static Node *parse_type_decl(Parser *p) {
    Token *t=xp(p,TK_KW_T,"'T'"); if(!t) return NULL;
    Node *n=mk(p,NODE_TYPE_DECL,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    Token *nt=cur(p); if(!xp(p,TK_TYPE_IDENT,"type name")){ sync(p);return n;}
    ch(p,n,mk(p,NODE_TYPE_IDENT,nt));
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(p,n,parse_field_list(p));
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    opt_semi(p);
    return n;
}

/*
 * parse_const_decl — parse a module-level constant declaration.
 *
 * Grammar:
 *   ConstDecl = IDENT '=' Literal ':' TypeExpr ';'
 *
 * AST node: NODE_CONST_DECL
 *   children[0] = NODE_IDENT    — constant name
 *   children[1] = literal node  — the constant value (must be a literal,
 *                                  not an arbitrary expression)
 *   children[2] = TypeExpr      — explicit type annotation
 *
 * Example: `MAX_SIZE = 1024 : u32;`
 *
 * Constants appear in the declaration ordering between type declarations
 * and function declarations.  They are identified by leading TK_IDENT
 * at the top level (not a keyword).
 *
 * Error recovery: calls sync() on missing '=' or ':'.
 */
/* ConstDecl = IDENT '=' LiteralExpr ':' TypeExpr ';' */
static Node *parse_const_decl(Parser *p) {
    Token *nt=cur(p); if(!xp(p,TK_IDENT,"constant name")) return NULL;
    Node *n=mk(p,NODE_CONST_DECL,nt); ch(p,n,mk(p,NODE_IDENT,nt));
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(p,n,parse_literal(p));
    if(!xp(p,TK_COLON,"':'")){ sync(p);return n;}
    ch(p,n,parse_type_expr(p));
    opt_semi(p);
    return n;
}

/*
 * parse_one_param — parse a single function parameter.
 *
 * Grammar:
 *   Param = IDENT ':' TypeExpr
 *
 * AST node: NODE_PARAM
 *   children[0] = NODE_IDENT — parameter name
 *   children[1] = TypeExpr   — parameter type
 *
 * This is a helper used by parse_func_decl.  The parameter node is
 * appended directly to the function declaration node (the `fn` argument)
 * rather than being returned, to simplify the calling loop.
 *
 * Error recovery: returns early if the parameter name is not an
 * identifier.  The ':' and type are expected but errors propagate
 * through xp().
 */
/* ParamList helper: parse one Param and add to func node */
static void parse_one_param(Parser *p, Node *fn) {
    Token *pt=cur(p); if(!xp(p,TK_IDENT,"parameter name")) return;
    Node *pm=mk(p,NODE_PARAM,pt); ch(p,pm,mk(p,NODE_IDENT,pt));
    if(xp(p,TK_COLON,"':'")) ch(p,pm,parse_type_expr(p));
    ch(p,fn,pm);
}

/*
 * parse_func_decl — parse a function declaration.
 *
 * Grammar:
 *   FuncDecl = 'F' '=' IDENT '(' ParamList ')' ':' ReturnSpec ('{' StmtList '}')?  ';'
 *   ParamList  = Param (';' Param)*
 *   ReturnSpec = TypeExpr ('!' TypeExpr)?
 *
 * AST node: NODE_FUNC_DECL
 *   children[0]        = NODE_IDENT      — function name
 *   children[1..n-3]   = NODE_PARAM      — parameter nodes (zero or more)
 *   children[n-2]      = NODE_RETURN_SPEC — return type specification
 *     children[0]      = TypeExpr for the success type
 *     children[1]      = TypeExpr for the error type (optional, after '!')
 *   children[n-1]      = NODE_STMT_LIST  — function body (absent for externs)
 *
 * If no '{' follows the return spec, the function is treated as an extern
 * (bodyless) declaration — no STMT_LIST child is added, and downstream
 * passes detect this by checking child_count.
 *
 * Parameters are separated by semicolons.  The parser peeks ahead to
 * avoid consuming a ';' that precedes ')'.
 *
 * Example:
 *   F=add(a: i32; b: i32): i32 {
 *       <a + b
 *   };
 *
 * Error recovery: calls sync() on missing '=', name, '(', or ':';
 * emits E2004 on unclosed ')' or '}'.
 */
/* FuncDecl = 'F' '=' IDENT '(' ParamList ')' ':' ReturnSpec '{' StmtList '}' ';' */
static Node *parse_func_decl(Parser *p) {
    Token *t=xp(p,TK_KW_F,"'F'"); if(!t) return NULL;
    Node *n=mk(p,NODE_FUNC_DECL,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    Token *nt=cur(p); if(!xp(p,TK_IDENT,"function name")){ sync(p);return n;}
    ch(p,n,mk(p,NODE_IDENT,nt));
    if(!xp(p,TK_LPAREN,"'('")){ sync(p);return n;}
    if(peek(p)!=TK_RPAREN){
        parse_one_param(p,n);
        while(peek(p)==TK_SEMICOLON&&p->pos+1<p->n&&p->toks[p->pos+1].kind!=TK_RPAREN){adv(p);parse_one_param(p,n);}
    }
    if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(!xp(p,TK_COLON,"':'")){ sync(p);return n;}
    /* ReturnSpec = TypeExpr ('!' TypeExpr)? */
    Token *rs=cur(p); Node *rspec=mk(p,NODE_RETURN_SPEC,rs);
    ch(p,rspec,parse_type_expr(p));
    if(peek(p)==TK_BANG){adv(p);ch(p,rspec,parse_type_expr(p));}
    ch(p,n,rspec);
    if(peek(p)==TK_LBRACE){
        xp(p,TK_LBRACE,"'{'");
        ch(p,n,parse_stmt_list(p,t));
        if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    }
    /* else: bodyless = extern declaration */
    opt_semi(p);
    return n;
}

/*
 * parse — public entry point: parse a token array into an AST.
 *
 * Grammar:
 *   SourceFile = ModuleDecl ImportDecl* TypeDecl* ConstDecl* FuncDecl* EOF
 *
 * AST node: NODE_PROGRAM (root)
 *   children[0]   = NODE_MODULE — module declaration
 *   children[1..] = NODE_IMPORT, NODE_TYPE_DECL, NODE_CONST_DECL,
 *                   NODE_FUNC_DECL in declaration order
 *
 * The parser enforces strict declaration ordering via a phase variable:
 *   phase 1 = imports (I=)
 *   phase 2 = types   (T=)
 *   phase 3 = consts  (bare IDENT at top level)
 *   phase 4 = funcs   (F=)
 * Moving to a later phase is allowed; moving backward emits E2001.
 *
 * The module declaration (M=) must appear first; its absence is a fatal
 * E2001 error but parsing continues for remaining diagnostics.
 *
 * Error handling:
 *   - Unrecognised top-level tokens emit E2002 and trigger sync().
 *   - A hard cap of 16 errors prevents runaway diagnostics.
 *   - Returns the (possibly incomplete) root node even on errors, so
 *     downstream passes can report additional diagnostics.
 *   - Returns NULL only on truly fatal conditions (NULL input, zero
 *     token count, or NULL arena).
 */
/*
 * is_decl_ident — check if current token is a lowercase declaration keyword
 * (m/f/i/t) followed by '=' at the top level in default mode.
 *
 * Returns the declaration phase (1=import 2=type 3=unused 4=func 5=module)
 * or 0 if not a declaration keyword.
 */
static int is_decl_ident(Parser *p) {
    if(p->profile!=PROFILE_DEFAULT) return 0;
    if(peek(p)!=TK_IDENT) return 0;
    if(peek_at(p,1)!=TK_EQ) return 0;
    Token *t=cur(p);
    if(t->len!=1) return 0;
    char c=p->src[t->start];
    if(c=='m') return 5;
    if(c=='i') return 1;
    if(c=='t') return 2;
    if(c=='f') return 4;
    return 0;
}

/*
 * consume_decl_kw — consume a lowercase declaration ident + '=' and
 * create a synthetic keyword consumption equivalent to xp(TK_KW_X).
 * Returns the token for the keyword ident, or NULL on failure.
 */
static Token *consume_decl_kw(Parser *p) {
    Token *t=adv(p); /* consume the single-letter ident */
    /* The '=' is consumed by the caller (parse_*_decl already does xp(TK_EQ)) */
    return t;
}

/* ── SourceFile = ModuleDecl ImportDecl* TypeDecl* ConstDecl* FuncDecl* EOF ── */
Node *
parse(Token *tokens, int count, const char *src, Arena *arena, Profile profile)
{
    if(!tokens||count<=0||!arena) return NULL;
    Parser p;
    p.toks=tokens; p.n=count; p.pos=0; p.src=src?src:""; p.a=arena; p.errs=0; p.profile=profile; p.in_sc=0;
    Token *first=cur(&p);
    Node *root=mk(&p,NODE_PROGRAM,first);
    /* Module declaration: M= or m= */
    if(peek(&p)==TK_KW_M||(is_decl_ident(&p)==5)){
        if(peek(&p)==TK_KW_M){
            ch(&p,root,parse_module_decl(&p));
        } else {
            /* lowercase m= : consume 'm', then manually parse like parse_module_decl */
            Token *mt=consume_decl_kw(&p);
            Node *n=mk(&p,NODE_MODULE,mt);
            if(!xp(&p,TK_EQ,"'='")){ sync(&p); ch(&p,root,n); }
            else { ch(&p,n,parse_module_path(&p)); opt_semi(&p); ch(&p,root,n); }
        }
    }
    else { diag_emit(DIAG_ERROR,E2001,first->start,first->line,first->col,"module declaration must appear first","fix",NULL); p.errs++; }
    int phase=1; /* 1=import 2=type 3=const 4=func */
    while(peek(&p)!=TK_EOF){
        Token *t=cur(&p); int cp;
        int di=is_decl_ident(&p);
        if(peek(&p)==TK_KW_I||(di==1))     cp=1;
        else if(peek(&p)==TK_KW_T||(di==2)) cp=2;
        else if(peek(&p)==TK_IDENT&&di==0)   cp=3;
        else if(peek(&p)==TK_KW_F||(di==4)) cp=4;
        else{eerr(&p,E2002,t,"unexpected token");sync(&p);if(p.errs>16)break;continue;}
        if(cp<phase){diag_emit(DIAG_ERROR,E2001,t->start,t->line,t->col,"declaration ordering violation","fix",NULL);p.errs++;}
        else if(cp>phase) phase=cp;
        Node *d;
        if(cp==1){
            if(peek(&p)==TK_KW_I) d=parse_import_decl(&p);
            else { Token *it=consume_decl_kw(&p);Node *n=mk(&p,NODE_IMPORT,it);
                if(!xp(&p,TK_EQ,"'='")){ sync(&p);d=n; }
                else {
                    Token *at=cur(&p); if(!xp(&p,TK_IDENT,"import alias")){ sync(&p);d=n; }
                    else { ch(&p,n,mk(&p,NODE_IDENT,at));
                        if(!xp(&p,TK_COLON,"':'")){ sync(&p);d=n; }
                        else { ch(&p,n,parse_module_path(&p));
                            if(peek(&p)==TK_STR_LIT){Token *vt=cur(&p);adv(&p);ch(&p,n,mk(&p,NODE_STR_LIT,vt));}
                            opt_semi(&p); d=n; }}}
            }
        }
        else if(cp==2){
            if(peek(&p)==TK_KW_T) d=parse_type_decl(&p);
            else { Token *tt=consume_decl_kw(&p);Node *n=mk(&p,NODE_TYPE_DECL,tt);
                if(!xp(&p,TK_EQ,"'='")){ sync(&p);d=n; }
                else {
                    /* In default mode, type name after t= can be $ident or uppercase TK_IDENT */
                    Token *nt=cur(&p);
                    if(peek(&p)==TK_DOLLAR){adv(&p);nt=cur(&p);
                        if(!xp(&p,TK_IDENT,"type name")){ sync(&p);d=n;goto next_decl;}
                    } else if(peek(&p)==TK_IDENT||peek(&p)==TK_TYPE_IDENT){
                        adv(&p);
                    } else {xp(&p,TK_IDENT,"type name");sync(&p);d=n;goto next_decl;}
                    ch(&p,n,mk(&p,NODE_TYPE_IDENT,nt));
                    if(!xp(&p,TK_LBRACE,"'{'")){ sync(&p);d=n; }
                    else { ch(&p,n,parse_field_list(&p));
                        if(!xp(&p,TK_RBRACE,"'}'"))eerr(&p,E2004,cur(&p),"unclosed delimiter");
                        opt_semi(&p); d=n; }}
            }
        }
        else if(cp==3) d=parse_const_decl(&p);
        else {
            if(peek(&p)==TK_KW_F) d=parse_func_decl(&p);
            else { Token *ft=consume_decl_kw(&p);Node *n=mk(&p,NODE_FUNC_DECL,ft);
                if(!xp(&p,TK_EQ,"'='")){ sync(&p);d=n; }
                else {
                    Token *nt=cur(&p); if(!xp(&p,TK_IDENT,"function name")){ sync(&p);d=n; }
                    else { ch(&p,n,mk(&p,NODE_IDENT,nt));
                        if(!xp(&p,TK_LPAREN,"'('")){ sync(&p);d=n; }
                        else {
                            if(peek(&p)!=TK_RPAREN){parse_one_param(&p,n);
                                while(peek(&p)==TK_SEMICOLON&&p.pos+1<p.n&&p.toks[p.pos+1].kind!=TK_RPAREN){adv(&p);parse_one_param(&p,n);}}
                            if(!xp(&p,TK_RPAREN,"')'"))eerr(&p,E2004,cur(&p),"unclosed delimiter");
                            if(!xp(&p,TK_COLON,"':'")){ sync(&p);d=n; }
                            else {
                                Token *rs=cur(&p); Node *rspec=mk(&p,NODE_RETURN_SPEC,rs);
                                ch(&p,rspec,parse_type_expr(&p));
                                if(peek(&p)==TK_BANG){adv(&p);ch(&p,rspec,parse_type_expr(&p));}
                                ch(&p,n,rspec);
                                if(peek(&p)==TK_LBRACE){xp(&p,TK_LBRACE,"'{'");
                                    ch(&p,n,parse_stmt_list(&p,ft));
                                    if(!xp(&p,TK_RBRACE,"'}'"))eerr(&p,E2004,cur(&p),"unclosed delimiter");}
                                opt_semi(&p); d=n; }}}}
            }
        }
        next_decl:
        ch(&p,root,d);
    }
    return root;
}
