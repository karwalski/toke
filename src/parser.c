/* parser.c — LL(1) recursive-descent parser for toke Profile 1. Story 1.2.2. */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "parser.h"

typedef struct { Token *toks; int n; int pos; const char *src; Arena *a; int errs; } Parser;

static TokenKind peek(Parser *p)  { return p->pos < p->n ? p->toks[p->pos].kind : TK_EOF; }
static Token    *cur(Parser *p)   { return &p->toks[p->pos < p->n ? p->pos : p->n-1]; }
static Token    *adv(Parser *p)   { Token *t = cur(p); if (p->pos < p->n-1) p->pos++; return t; }

static Node *mk(Parser *p, NodeKind k, Token *t) {
    Node *n = (Node *)arena_alloc(p->a, (int)sizeof(Node));
    if (!n) return NULL;
    n->kind=k; n->start=t->start; n->line=t->line; n->col=t->col;
    n->tok_start=t->start; n->tok_len=t->len; n->child_count=0; n->op=TK_EOF;
    return n;
}
static void ch(Node *par, Node *c) { if (par&&c&&par->child_count<NODE_MAX_CHILDREN) par->children[par->child_count++]=c; }
static int  teq(Parser *p, Token *t, const char *s) { int l=(int)strlen(s); return t->len==l&&!memcmp(p->src+t->start,s,(size_t)l); }
static void eerr(Parser *p, int code, Token *t, const char *msg) { p->errs++; diag_emit(DIAG_ERROR,code,t->start,t->line,t->col,msg,"fix",NULL); }
static void sync(Parser *p) { while(peek(p)!=TK_EOF){TokenKind k=peek(p);adv(p);if(k==TK_SEMICOLON||k==TK_RBRACE)return;} }
/* opt_semi — consume ';' if present; silent if next token is '}' or EOF
 * (trailing semicolon elision: last stmt in a block need not carry ';'). */
static void opt_semi(Parser *p) {
    if(peek(p)==TK_SEMICOLON){adv(p);return;}
    if(peek(p)==TK_RBRACE||peek(p)==TK_EOF) return;
    eerr(p,E2003,cur(p),"missing semicolon");
}
static Token *xp(Parser *p, TokenKind k, const char *w) {
    (void)w; if(peek(p)==k) return adv(p);
    eerr(p, peek(p)==TK_EOF?E2004:E2002, cur(p), "unexpected token"); return NULL;
}

static Node *parse_expr(Parser *p);
static Node *parse_stmt_list(Parser *p, Token *ref);
static Node *parse_type_expr(Parser *p);

/* ── Type expressions ─────────────────────────────────────────────── */

static int is_scalar(Parser *p, Token *t) {
    if (t->kind!=TK_IDENT&&t->kind!=TK_TYPE_IDENT) return 0;
    static const char *sc[]={"u8","u16","u32","u64","i8","i16","i32","i64","f32","f64","bool","Str","Byte",NULL};
    for(int i=0;sc[i];i++) if(teq(p,t,sc[i])) return 1;
    return 0;
}

/* FuncTypeExpr = '(' TypeExpr (';' TypeExpr)* ')' ':' TypeExpr */
static Node *parse_func_type(Parser *p) {
    Token *t=xp(p,TK_LPAREN,"'('"); if(!t) return NULL;
    Node *n=mk(p,NODE_FUNC_TYPE,t);
    if(peek(p)!=TK_RPAREN){ch(n,parse_type_expr(p));while(peek(p)==TK_SEMICOLON){adv(p);ch(n,parse_type_expr(p));}}
    if(!xp(p,TK_RPAREN,"')'")) eerr(p,E2004,cur(p),"unclosed delimiter");
    if(xp(p,TK_COLON,"':'")) ch(n,parse_type_expr(p));
    return n;
}

/* TypeExpr = '*' TypeExpr | ScalarType | TYPE_IDENT | '[' TypeExpr ']' | FuncTypeExpr */
static Node *parse_type_expr(Parser *p) {
    if(peek(p)==TK_STAR){Token *t=adv(p);Node *n=mk(p,NODE_PTR_TYPE,t);ch(n,parse_type_expr(p));return n;}
    if(peek(p)==TK_LBRACKET){Token *t=xp(p,TK_LBRACKET,"'['");Node *n=mk(p,NODE_ARRAY_TYPE,t);ch(n,parse_type_expr(p));if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;}
    if(peek(p)==TK_LPAREN) return parse_func_type(p);
    Token *t=cur(p);
    if(is_scalar(p,t)){Node *n=mk(p,NODE_TYPE_EXPR,t);adv(p);return n;}
    if(peek(p)==TK_TYPE_IDENT){Node *n=mk(p,NODE_TYPE_IDENT,t);adv(p);return n;}
    eerr(p,E2002,t,"unexpected token"); return NULL;
}

/* ── Literals ─────────────────────────────────────────────────────── */
static Node *parse_literal(Parser *p) {
    Token *t=cur(p); NodeKind k;
    switch(peek(p)){case TK_INT_LIT:k=NODE_INT_LIT;break;case TK_FLOAT_LIT:k=NODE_FLOAT_LIT;break;
    case TK_STR_LIT:k=NODE_STR_LIT;break;case TK_BOOL_LIT:k=NODE_BOOL_LIT;break;
    default:eerr(p,E2002,t,"unexpected token");return NULL;}
    adv(p); return mk(p,k,t);
}

/* ── Primary ──────────────────────────────────────────────────────── */
static Node *parse_primary(Parser *p) {
    Token *t=cur(p);
    if(peek(p)==TK_IDENT){adv(p);return mk(p,NODE_IDENT,t);}
    if(peek(p)==TK_TYPE_IDENT){
        adv(p);
        if(peek(p)==TK_LBRACE){ /* StructLit = TypeName '{' FieldInit (';' FieldInit)* '}' */
            Node *n=mk(p,NODE_STRUCT_LIT,t); xp(p,TK_LBRACE,"'{'");
            while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF){
                Token *ft=cur(p); if(!xp(p,TK_IDENT,"field name")){sync(p);break;}
                Node *fi=mk(p,NODE_FIELD_INIT,ft); if(!xp(p,TK_COLON,"':'")){ sync(p);break;}
                ch(fi,parse_expr(p)); ch(n,fi); if(peek(p)==TK_SEMICOLON)adv(p);else break;
            }
            if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter"); return n;
        }
        return mk(p,NODE_TYPE_IDENT,t);
    }
    if(peek(p)==TK_LPAREN){adv(p);Node *e=parse_expr(p);if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");return e;}
    if(peek(p)==TK_LBRACKET){ /* ArrayLit = '[' (Expr (';' Expr)*)? ']' */
        Token *lt=xp(p,TK_LBRACKET,"'['"); Node *n=mk(p,NODE_ARRAY_LIT,lt);
        if(peek(p)!=TK_RBRACKET){ch(n,parse_expr(p));while(peek(p)==TK_SEMICOLON){adv(p);ch(n,parse_expr(p));}}
        if(!xp(p,TK_RBRACKET,"']'"))eerr(p,E2004,cur(p),"unclosed delimiter"); return n;
    }
    return parse_literal(p);
}

/* ── Expression precedence chain ──────────────────────────────────── */

/* PostfixExpr = PrimaryExpr ('.' IDENT)* */
static Node *parse_postfix(Parser *p) {
    Node *l=parse_primary(p);
    while(peek(p)==TK_DOT){Token *d=adv(p);Token *f=cur(p);if(!xp(p,TK_IDENT,"field"))break;Node *n=mk(p,NODE_FIELD_EXPR,d);ch(n,l);ch(n,mk(p,NODE_IDENT,f));l=n;}
    return l;
}

/* CallExpr = PostfixExpr ('(' ArgList ')')* */
static Node *parse_call(Parser *p) {
    Node *l=parse_postfix(p);
    while(peek(p)==TK_LPAREN){Token *op=adv(p);Node *c=mk(p,NODE_CALL_EXPR,op);ch(c,l);
        if(peek(p)!=TK_RPAREN){ch(c,parse_expr(p));while(peek(p)==TK_SEMICOLON){adv(p);ch(c,parse_expr(p));}}
        if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");l=c;}
    return l;
}

/* CastExpr = CallExpr ('as' TypeExpr)?  /  PropagateExpr = CastExpr ('!' TypeExpr)? */
static Node *parse_cast_prop(Parser *p) {
    Node *l=parse_call(p);
    if(peek(p)==TK_KW_AS){Token *t=adv(p);Node *n=mk(p,NODE_CAST_EXPR,t);ch(n,l);ch(n,parse_type_expr(p));l=n;}
    if(peek(p)==TK_BANG){Token *t=adv(p);Node *n=mk(p,NODE_PROPAGATE_EXPR,t);ch(n,l);ch(n,parse_type_expr(p));l=n;}
    return l;
}

/* UnaryExpr = ('-'|'!') UnaryExpr | PropagateExpr */
static Node *parse_unary(Parser *p) {
    if(peek(p)==TK_MINUS||peek(p)==TK_BANG){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_UNARY_EXPR,t);n->op=op;ch(n,parse_unary(p));return n;}
    return parse_cast_prop(p);
}

/* MulExpr = UnaryExpr (('*'|'/') UnaryExpr)* */
static Node *parse_mul(Parser *p) {
    Node *l=parse_unary(p);
    while(peek(p)==TK_STAR||peek(p)==TK_SLASH){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(n,l);ch(n,parse_unary(p));l=n;}
    return l;
}

/* AddExpr = MulExpr (('+'|'-') MulExpr)* */
static Node *parse_add(Parser *p) {
    Node *l=parse_mul(p);
    while(peek(p)==TK_PLUS||peek(p)==TK_MINUS){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(n,l);ch(n,parse_mul(p));l=n;}
    return l;
}

/* CompareExpr = AddExpr (('<'|'>'|'=') AddExpr)? */
static Node *parse_compare(Parser *p) {
    Node *l=parse_add(p);
    if(peek(p)==TK_LT||peek(p)==TK_GT||peek(p)==TK_EQ){Token *t=cur(p);TokenKind op=adv(p)->kind;Node *n=mk(p,NODE_BINARY_EXPR,t);n->op=op;ch(n,l);ch(n,parse_add(p));return n;}
    return l;
}

/* MatchArm = TYPE_IDENT ':' IDENT Expr  /  MatchExpr = CompareExpr ('|' '{' MatchArmList '}')? */
static Node *parse_match_arm(Parser *p) {
    Token *pt=cur(p); if(!xp(p,TK_TYPE_IDENT,"match pattern")) return NULL;
    Node *arm=mk(p,NODE_MATCH_ARM,pt); ch(arm,mk(p,NODE_TYPE_IDENT,pt));
    if(!xp(p,TK_COLON,"':'")){ sync(p);return arm;}
    Token *bt=cur(p); if(!xp(p,TK_IDENT,"binding name")){sync(p);return arm;}
    ch(arm,mk(p,NODE_IDENT,bt)); ch(arm,parse_expr(p)); return arm;
}

static Node *parse_expr(Parser *p) {
    Node *l=parse_compare(p);
    if(peek(p)!=TK_PIPE) return l;
    Token *t=adv(p); Node *n=mk(p,NODE_MATCH_STMT,t); ch(n,l);
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(n,parse_match_arm(p));
    while(peek(p)==TK_SEMICOLON){adv(p);if(peek(p)==TK_RBRACE)break;ch(n,parse_match_arm(p));}
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    return n;
}

/* ── Statements ───────────────────────────────────────────────────── */

/* LoopStmt = 'lp' '(' LoopInit ';' Expr ';' LoopStep ')' '{' StmtList '}' */
static Node *parse_loop_stmt(Parser *p) {
    Token *t=xp(p,TK_KW_LP,"'lp'"); if(!t) return NULL;
    Node *n=mk(p,NODE_LOOP_STMT,t);
    if(!xp(p,TK_LPAREN,"'('")){ sync(p);return n;}
    /* LoopInit = ('let' IDENT | IDENT) '=' Expr */
    Token *it=cur(p); Node *ini=mk(p,NODE_LOOP_INIT,it);
    if(peek(p)==TK_KW_LET){ini->op=TK_KW_LET;adv(p);}
    Token *nt=cur(p); if(xp(p,TK_IDENT,"identifier")){ch(ini,mk(p,NODE_IDENT,nt));}
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(ini,parse_expr(p)); ch(n,ini);
    if(!xp(p,TK_SEMICOLON,"';'")){ sync(p);return n;}
    ch(n,parse_expr(p));
    if(!xp(p,TK_SEMICOLON,"';'")){ sync(p);return n;}
    /* LoopStep = IDENT '=' Expr */
    Token *st=cur(p); if(!xp(p,TK_IDENT,"identifier")){ sync(p);return n;}
    Node *step=mk(p,NODE_ASSIGN_STMT,st); ch(step,mk(p,NODE_IDENT,st));
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(step,parse_expr(p)); ch(n,step);
    if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(n,parse_stmt_list(p,t));
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(peek(p)==TK_SEMICOLON) adv(p);  /* optional trailing ';' after loop block */
    return n;
}

/* IfStmt = 'if' '(' Expr ')' '{' StmtList '}' ('el' '{' StmtList '}')? */
static Node *parse_if_stmt(Parser *p) {
    Token *t=xp(p,TK_KW_IF,"'if'"); if(!t) return NULL;
    Node *n=mk(p,NODE_IF_STMT,t);
    if(!xp(p,TK_LPAREN,"'('")){ sync(p);return n;}
    ch(n,parse_expr(p));
    if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(n,parse_stmt_list(p,t));
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(peek(p)==TK_KW_EL){adv(p);
        if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
        ch(n,parse_stmt_list(p,t));
        if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");}
    if(peek(p)==TK_SEMICOLON) adv(p);  /* optional trailing ';' after if/el block */
    return n;
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
        if(!xp(p,TK_EQ,"'='")){ sync(p);return NULL;}
        int mut=(peek(p)==TK_KW_MUT);
        Node *n=mk(p,mut?NODE_MUT_BIND_STMT:NODE_BIND_STMT,t); ch(n,mk(p,NODE_IDENT,nt));
        if(mut){adv(p);if(!xp(p,TK_DOT,"'.'")){ sync(p);return n;}}
        ch(n,parse_expr(p));
        opt_semi(p);
        return n;}
    case TK_KW_BR:{adv(p);Node *n=mk(p,NODE_BREAK_STMT,t);opt_semi(p);return n;}
    case TK_KW_RT:
    case TK_LT:{adv(p);Node *n=mk(p,NODE_RETURN_STMT,t);ch(n,parse_expr(p));opt_semi(p);return n;}
    case TK_KW_IF: return parse_if_stmt(p);
    case TK_KW_LP: return parse_loop_stmt(p);
    case TK_LBRACE:
        /* ArenaStmt: '{' IDENT('arena') StmtList '}' — lookahead into block */
        if(p->pos+1<p->n&&p->toks[p->pos+1].kind==TK_IDENT&&teq(p,&p->toks[p->pos+1],"arena")){
            adv(p);adv(p);Node *n=mk(p,NODE_ARENA_STMT,t);
            ch(n,parse_stmt_list(p,t));
            if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");return n;}
        eerr(p,E2002,t,"unexpected token");sync(p);return NULL;
    case TK_IDENT:
        /* AssignStmt: IDENT '=' Expr ';' — one-token ahead check on '=' */
        if(p->pos+1<p->n&&p->toks[p->pos+1].kind==TK_EQ){
            adv(p);adv(p);Node *n=mk(p,NODE_ASSIGN_STMT,t);ch(n,mk(p,NODE_IDENT,t));
            ch(n,parse_expr(p));opt_semi(p);return n;}
        /* fall through to ExprStmt */
    default:{Node *n=mk(p,NODE_EXPR_STMT,t);ch(n,parse_expr(p));opt_semi(p);return n;}
    }
}

static Node *parse_stmt_list(Parser *p, Token *ref) {
    Node *n=mk(p,NODE_STMT_LIST,ref);
    while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF){int sv=p->pos;Node *s=parse_stmt(p);if(s){ch(n,s);}else if(p->pos==sv){sync(p);}}
    return n;
}

/* ── Top-level declarations ───────────────────────────────────────── */

/* ModulePath = IDENT ('.' IDENT)* */
static Node *parse_module_path(Parser *p) {
    Token *t=cur(p); if(!xp(p,TK_IDENT,"module path segment")) return NULL;
    Node *n=mk(p,NODE_MODULE_PATH,t); ch(n,mk(p,NODE_IDENT,t));
    while(peek(p)==TK_DOT){adv(p);Token *s=cur(p);if(!xp(p,TK_IDENT,"segment"))break;ch(n,mk(p,NODE_IDENT,s));}
    return n;
}

/* ModuleDecl = 'M' '=' ModulePath ';' */
static Node *parse_module_decl(Parser *p) {
    Token *t=xp(p,TK_KW_M,"'M'"); if(!t) return NULL;
    Node *n=mk(p,NODE_MODULE,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(n,parse_module_path(p));
    opt_semi(p);
    return n;
}

/* ImportDecl = 'I' '=' IDENT ':' ModulePath (STR_LIT)? ';' */
static Node *parse_import_decl(Parser *p) {
    Token *t=xp(p,TK_KW_I,"'I'"); if(!t) return NULL;
    Node *n=mk(p,NODE_IMPORT,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    Token *at=cur(p); if(!xp(p,TK_IDENT,"import alias")){ sync(p);return n;}
    ch(n,mk(p,NODE_IDENT,at));
    if(!xp(p,TK_COLON,"':'")){ sync(p);return n;}
    ch(n,parse_module_path(p));
    /* Optional version string: I=alias:module.path "1.2"; */
    if(peek(p)==TK_STR_LIT){
        Token *vt=cur(p); adv(p);
        ch(n,mk(p,NODE_STR_LIT,vt));
    }
    opt_semi(p);
    return n;
}

/* FieldList = Field (';' Field)*  (used by TypeDecl) */
static Node *parse_field_list(Parser *p) {
    Node *n=mk(p,NODE_STMT_LIST,cur(p));
    do {
        Token *ft=cur(p); TokenKind fk=peek(p);
        if(fk!=TK_IDENT&&fk!=TK_TYPE_IDENT){ eerr(p,E2002,ft,"unexpected token");break;}
        adv(p); Node *f=mk(p,NODE_FIELD,ft);
        if(!xp(p,TK_COLON,"':'")){ sync(p);break;}
        ch(f,parse_type_expr(p)); ch(n,f);
        if(peek(p)!=TK_SEMICOLON) break;
        if(p->pos+1<p->n&&p->toks[p->pos+1].kind==TK_RBRACE) break;
        adv(p);
    } while(peek(p)!=TK_RBRACE&&peek(p)!=TK_EOF);
    return n;
}

/* TypeDecl = 'T' '=' TypeName '{' FieldList '}' ';' */
static Node *parse_type_decl(Parser *p) {
    Token *t=xp(p,TK_KW_T,"'T'"); if(!t) return NULL;
    Node *n=mk(p,NODE_TYPE_DECL,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    Token *nt=cur(p); if(!xp(p,TK_TYPE_IDENT,"type name")){ sync(p);return n;}
    ch(n,mk(p,NODE_TYPE_IDENT,nt));
    if(!xp(p,TK_LBRACE,"'{'")){ sync(p);return n;}
    ch(n,parse_field_list(p));
    if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    opt_semi(p);
    return n;
}

/* ConstDecl = IDENT '=' LiteralExpr ':' TypeExpr ';' */
static Node *parse_const_decl(Parser *p) {
    Token *nt=cur(p); if(!xp(p,TK_IDENT,"constant name")) return NULL;
    Node *n=mk(p,NODE_CONST_DECL,nt); ch(n,mk(p,NODE_IDENT,nt));
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    ch(n,parse_literal(p));
    if(!xp(p,TK_COLON,"':'")){ sync(p);return n;}
    ch(n,parse_type_expr(p));
    opt_semi(p);
    return n;
}

/* ParamList helper: parse one Param and add to func node */
static void parse_one_param(Parser *p, Node *fn) {
    Token *pt=cur(p); if(!xp(p,TK_IDENT,"parameter name")) return;
    Node *pm=mk(p,NODE_PARAM,pt); ch(pm,mk(p,NODE_IDENT,pt));
    if(xp(p,TK_COLON,"':'")) ch(pm,parse_type_expr(p));
    ch(fn,pm);
}

/* FuncDecl = 'F' '=' IDENT '(' ParamList ')' ':' ReturnSpec '{' StmtList '}' ';' */
static Node *parse_func_decl(Parser *p) {
    Token *t=xp(p,TK_KW_F,"'F'"); if(!t) return NULL;
    Node *n=mk(p,NODE_FUNC_DECL,t);
    if(!xp(p,TK_EQ,"'='")){ sync(p);return n;}
    Token *nt=cur(p); if(!xp(p,TK_IDENT,"function name")){ sync(p);return n;}
    ch(n,mk(p,NODE_IDENT,nt));
    if(!xp(p,TK_LPAREN,"'('")){ sync(p);return n;}
    if(peek(p)!=TK_RPAREN){
        parse_one_param(p,n);
        while(peek(p)==TK_SEMICOLON&&p->pos+1<p->n&&p->toks[p->pos+1].kind!=TK_RPAREN){adv(p);parse_one_param(p,n);}
    }
    if(!xp(p,TK_RPAREN,"')'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    if(!xp(p,TK_COLON,"':'")){ sync(p);return n;}
    /* ReturnSpec = TypeExpr ('!' TypeExpr)? */
    Token *rs=cur(p); Node *rspec=mk(p,NODE_RETURN_SPEC,rs);
    ch(rspec,parse_type_expr(p));
    if(peek(p)==TK_BANG){adv(p);ch(rspec,parse_type_expr(p));}
    ch(n,rspec);
    if(peek(p)==TK_LBRACE){
        xp(p,TK_LBRACE,"'{'");
        ch(n,parse_stmt_list(p,t));
        if(!xp(p,TK_RBRACE,"'}'"))eerr(p,E2004,cur(p),"unclosed delimiter");
    }
    /* else: bodyless = extern declaration */
    opt_semi(p);
    return n;
}

/* ── SourceFile = ModuleDecl ImportDecl* TypeDecl* ConstDecl* FuncDecl* EOF ── */
Node *
parse(Token *tokens, int count, const char *src, Arena *arena)
{
    if(!tokens||count<=0||!arena) return NULL;
    Parser p={tokens,count,0,src?src:"",arena,0};
    Token *first=cur(&p);
    Node *root=mk(&p,NODE_PROGRAM,first);
    if(peek(&p)==TK_KW_M) ch(root,parse_module_decl(&p));
    else { diag_emit(DIAG_ERROR,E2001,first->start,first->line,first->col,"module declaration must appear first","fix",NULL); p.errs++; }
    int phase=1; /* 1=import 2=type 3=const 4=func */
    while(peek(&p)!=TK_EOF){
        Token *t=cur(&p); int cp;
        if(peek(&p)==TK_KW_I)     cp=1;
        else if(peek(&p)==TK_KW_T) cp=2;
        else if(peek(&p)==TK_IDENT)cp=3;
        else if(peek(&p)==TK_KW_F) cp=4;
        else{eerr(&p,E2002,t,"unexpected token");sync(&p);if(p.errs>16)break;continue;}
        if(cp<phase){diag_emit(DIAG_ERROR,E2001,t->start,t->line,t->col,"declaration ordering violation","fix",NULL);p.errs++;}
        else if(cp>phase) phase=cp;
        Node *d;
        if(cp==1)      d=parse_import_decl(&p);
        else if(cp==2) d=parse_type_decl(&p);
        else if(cp==3) d=parse_const_decl(&p);
        else           d=parse_func_decl(&p);
        ch(root,d);
    }
    return root;
}
