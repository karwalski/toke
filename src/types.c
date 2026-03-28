/* types.c — Type checker for the toke reference compiler. Story 1.2.5. */
#include "types.h"
#include <string.h>
#include <stdio.h>

static const char *ty_intern(Arena *arena, const char *s) {
    int len = (int)strlen(s);
    char *p = (char *)arena_alloc(arena, len + 1);
    if (!p) return s;
    memcpy(p, s, (size_t)len + 1);
    return p;
}

static Type *mk_type(Arena *arena, TypeKind k) {
    Type *t = (Type *)arena_alloc(arena, (int)sizeof(Type));
    if (!t) return NULL;
    t->kind = k; t->name = NULL; t->elem = NULL;
    t->field_count = 0; t->field_names = NULL; t->field_types = NULL;
    return t;
}

static const char *type_name(const Type *t) {
    if (!t) return "unknown";
    switch (t->kind) {
    case TY_VOID: return "void"; case TY_BOOL: return "bool"; case TY_I64: return "i64";
    case TY_U64: return "u64";   case TY_F64: return "f64";   case TY_STR: return "str";
    case TY_STRUCT: return t->name?t->name:"struct"; case TY_ARRAY: return "array";
    case TY_FUNC: return "func"; case TY_ERROR_TYPE: return t->elem?type_name(t->elem):"error";
    default: return "unknown";
    }
}

static int types_equal(const Type *a, const Type *b) {
    if (!a||!b) return 0; if (a==b) return 1; if (a->kind!=b->kind) return 0;
    if (a->kind==TY_STRUCT) return a->name&&b->name&&strcmp(a->name,b->name)==0;
    if (a->kind==TY_ARRAY||a->kind==TY_FUNC||a->kind==TY_ERROR_TYPE) {
        /* TY_UNKNOWN elem is compatible with any elem (e.g. empty array literal []). */
        if ((a->elem&&a->elem->kind==TY_UNKNOWN)||(b->elem&&b->elem->kind==TY_UNKNOWN)) return 1;
        return types_equal(a->elem,b->elem);
    }
    return 1;
}

static int is_numeric(const Type *t) {
    return t && (t->kind==TY_I64 || t->kind==TY_U64 || t->kind==TY_F64);
}

static Decl *tc_lookup(const Scope *s, const char *name, int len) {
    for (; s; s = s->parent)
        for (Decl *d = s->head; d; d = d->next)
            if (d->name_len==len && memcmp(d->name,name,(size_t)len)==0)
                return d;
    return NULL;
}

#define TOKSTR(buf,src,node) do { \
    int _l=(node)->tok_len<(int)(sizeof(buf)-1)?(node)->tok_len:(int)(sizeof(buf)-1); \
    memcpy(buf,(src)+(node)->tok_start,(size_t)_l); buf[_l]='\0'; \
} while(0)

typedef struct { TypeEnv *env; const char *src;
                 Type *fn_ret; int had_error; } Ctx;

static Type *infer(Ctx *cx, const Node *node);

static Type *resolve_type(Ctx *cx, const Node *n) {
    if (!n) return mk_type(cx->env->arena, TY_UNKNOWN);
    char nb[128]; TOKSTR(nb, cx->src, n);
    if (n->kind == NODE_ARRAY_TYPE) {
        Type *at = mk_type(cx->env->arena, TY_ARRAY);
        if (at && n->child_count > 0) at->elem = resolve_type(cx, n->children[0]);
        return at ? at : mk_type(cx->env->arena, TY_UNKNOWN);
    }
    if (strcmp(nb,"void")==0) return mk_type(cx->env->arena, TY_VOID);
    if (strcmp(nb,"bool")==0) return mk_type(cx->env->arena, TY_BOOL);
    if (strcmp(nb,"i64") ==0) return mk_type(cx->env->arena, TY_I64);
    if (strcmp(nb,"u64") ==0) return mk_type(cx->env->arena, TY_U64);
    if (strcmp(nb,"f64") ==0) return mk_type(cx->env->arena, TY_F64);
    if (strcmp(nb,"str") ==0) return mk_type(cx->env->arena, TY_STR);
    Decl *d = tc_lookup(cx->env->names->module_scope, nb, (int)strlen(nb));
    if (d && d->def_node && d->def_node->kind == NODE_TYPE_DECL) {
        const Node *decl = d->def_node;
        Type *st = mk_type(cx->env->arena, TY_STRUCT);
        if (!st) return mk_type(cx->env->arena, TY_UNKNOWN);
        const Node *nn = decl->child_count>0?decl->children[0]:NULL;
        if (nn) { char snb[128]; TOKSTR(snb,cx->src,nn); st->name=ty_intern(cx->env->arena,snb); }
        int fc=0;
        for (int i=1;i<decl->child_count;i++)
            if (decl->children[i]&&decl->children[i]->kind==NODE_FIELD) fc++;
        if (fc>0) {
            st->field_names=(const char**)arena_alloc(cx->env->arena,fc*(int)sizeof(char*));
            st->field_types=(Type**)arena_alloc(cx->env->arena,fc*(int)sizeof(Type*));
            if (st->field_names&&st->field_types) {
                st->field_count=fc; int fi=0;
                for (int i=1;i<decl->child_count&&fi<fc;i++) {
                    const Node *f=decl->children[i];
                    if (!f||f->kind!=NODE_FIELD) continue;
                    char fnb[128]={0};
                    const Node *fn=f->child_count>0?f->children[0]:NULL;
                    if (fn) TOKSTR(fnb,cx->src,fn);
                    st->field_names[fi]=ty_intern(cx->env->arena,fnb);
                    st->field_types[fi]=f->child_count>1
                        ?resolve_type(cx,f->children[1])
                        :mk_type(cx->env->arena,TY_UNKNOWN);
                    fi++;
                }
            }
        }
        return st;
    }
    return mk_type(cx->env->arena, TY_UNKNOWN);
}

static void emit_mm(Ctx *cx, const Node *n, const Type *exp,
                    const Type *got, const char *fix) {
    char msg[256];
    snprintf(msg,sizeof(msg),"type mismatch: expected '%s', got '%s'",
             type_name(exp),type_name(got));
    if (fix)
        diag_emit(DIAG_ERROR,E4031,n->start,n->line,n->col,msg,"fix",fix,(const char*)NULL);
    else
        diag_emit(DIAG_ERROR,E4031,n->start,n->line,n->col,msg,"fix",(const char*)NULL);
    cx->had_error=1;
}

static Type *infer(Ctx *cx, const Node *node) {
    if (!node) return mk_type(cx->env->arena, TY_UNKNOWN);
    Arena *A = cx->env->arena;
    switch (node->kind) {
    case NODE_INT_LIT:   return mk_type(A,TY_I64);
    case NODE_FLOAT_LIT: return mk_type(A,TY_F64);
    case NODE_STR_LIT:   return mk_type(A,TY_STR);
    case NODE_BOOL_LIT:  return mk_type(A,TY_BOOL);

    case NODE_STRUCT_LIT:
        /* node token is the TYPE_IDENT — resolve_type looks it up in scope. */
        return resolve_type(cx, node);

    case NODE_ARRAY_LIT: {
        /* Infer element type from first element; return [elem_type]. */
        Type *at = mk_type(A, TY_ARRAY);
        if (!at) return mk_type(A, TY_UNKNOWN);
        if (node->child_count > 0)
            at->elem = infer(cx, node->children[0]);
        else
            at->elem = mk_type(A, TY_UNKNOWN);
        return at;
    }

    case NODE_IDENT: {
        char nb[128]; TOKSTR(nb,cx->src,node);
        Decl *d=tc_lookup(cx->env->names->module_scope,nb,(int)strlen(nb));
        if (!d||!d->def_node) return mk_type(A,TY_UNKNOWN);
        const Node *def=d->def_node;
        if ((def->kind==NODE_BIND_STMT||def->kind==NODE_MUT_BIND_STMT)) {
            if (def->child_count>1&&def->children[1]) return resolve_type(cx,def->children[1]);
            if (def->child_count>2&&def->children[2]) return infer(cx,def->children[2]);
        }
        if (def->kind==NODE_PARAM&&def->child_count>1&&def->children[1])
            return resolve_type(cx,def->children[1]);
        if (def->kind==NODE_FUNC_DECL) return mk_type(A,TY_FUNC);
        return mk_type(A,TY_UNKNOWN);
    }

    case NODE_UNARY_EXPR: {
        Type *op=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        if (node->op==TK_MINUS&&op->kind!=TY_UNKNOWN
            &&op->kind!=TY_I64&&op->kind!=TY_F64) {
            char msg[128];
            snprintf(msg,sizeof(msg),"type mismatch: expected 'i64' or 'f64', got '%s'",type_name(op));
            diag_emit(DIAG_ERROR,E4031,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
        }
        return op;
    }

    case NODE_BINARY_EXPR: {
        Type *l=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        Type *r=node->child_count>1?infer(cx,node->children[1]):mk_type(A,TY_UNKNOWN);
        if (l->kind==TY_UNKNOWN||r->kind==TY_UNKNOWN) return mk_type(A,TY_UNKNOWN);
        int arith=(node->op==TK_PLUS||node->op==TK_MINUS||node->op==TK_STAR||node->op==TK_SLASH);
        int cmp  =(node->op==TK_LT  ||node->op==TK_GT  ||node->op==TK_EQ);
        if (arith||cmp) {
            if ((arith&&(!is_numeric(l)||!types_equal(l,r)))||(cmp&&!types_equal(l,r))) {
                char fix[64];
                snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(l));
                emit_mm(cx,node,l,r,fix);
                return cmp?mk_type(A,TY_BOOL):mk_type(A,TY_UNKNOWN);
            }
        }
        return cmp?mk_type(A,TY_BOOL):l;
    }

    case NODE_CALL_EXPR: {
        if (node->child_count<1) return mk_type(A,TY_UNKNOWN);
        char nb[128]; TOKSTR(nb,cx->src,node->children[0]);
        Decl *d=tc_lookup(cx->env->names->module_scope,nb,(int)strlen(nb));
        if (!d||!d->def_node||d->def_node->kind!=NODE_FUNC_DECL) {
            for (int i=1;i<node->child_count;i++) infer(cx,node->children[i]);
            return mk_type(A,TY_UNKNOWN);
        }
        const Node *fn=d->def_node; int pi=0;
        for (int i=0;i<fn->child_count;i++) {
            const Node *ch=fn->children[i];
            if (!ch||ch->kind!=NODE_PARAM) continue;
            int ai=1+pi;
            if (ai<node->child_count) {
                Type *at=infer(cx,node->children[ai]);
                Type *pt=ch->child_count>1?resolve_type(cx,ch->children[1]):mk_type(A,TY_UNKNOWN);
                if (at->kind!=TY_UNKNOWN&&pt->kind!=TY_UNKNOWN&&!types_equal(at,pt)) {
                    char fix[128];
                    snprintf(fix,sizeof(fix),"cast argument to %s using 'as'",type_name(pt));
                    emit_mm(cx,node->children[ai],pt,at,fix);
                }
            }
            pi++;
        }
        for (int i=1+pi;i<node->child_count;i++) infer(cx,node->children[i]);
        for (int i=0;i<fn->child_count;i++) {
            const Node *ch=fn->children[i];
            if (ch&&ch->kind==NODE_RETURN_SPEC&&ch->child_count>0)
                return resolve_type(cx,ch->children[0]);
        }
        return mk_type(A,TY_VOID);
    }

    case NODE_CAST_EXPR:
        if (node->child_count>1) { infer(cx,node->children[0]); return resolve_type(cx,node->children[1]); }
        return mk_type(A,TY_UNKNOWN);

    case NODE_PROPAGATE_EXPR: {
        Type *inner=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        if (!cx->fn_ret||cx->fn_ret->kind!=TY_ERROR_TYPE||
            (inner->kind!=TY_UNKNOWN&&inner->kind!=TY_ERROR_TYPE)) {
            diag_emit(DIAG_ERROR,E3020,node->start,node->line,node->col,
                "! applied to a non-error-union value; function must return T!Err",
                "fix",(const char*)NULL);
            cx->had_error=1; return mk_type(A,TY_UNKNOWN);
        }
        return (inner->kind==TY_ERROR_TYPE&&inner->elem)?inner->elem:mk_type(A,TY_UNKNOWN);
    }

    case NODE_FIELD_EXPR: {
        Type *base=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        if (base->kind!=TY_STRUCT) return mk_type(A,TY_UNKNOWN);
        if (node->child_count<2||!node->children[1]) return mk_type(A,TY_UNKNOWN);
        char fname[128]; TOKSTR(fname,cx->src,node->children[1]);
        for (int i=0;i<base->field_count;i++)
            if (base->field_names[i]&&strcmp(base->field_names[i],fname)==0)
                return base->field_types[i];
        char msg[256];
        snprintf(msg,sizeof(msg),"struct '%s' has no field '%s'",
                 base->name?base->name:"?",fname);
        diag_emit(DIAG_ERROR,E4025,node->start,node->line,node->col,msg,"fix",(const char*)NULL);
        cx->had_error=1; return mk_type(A,TY_UNKNOWN);
    }

    case NODE_BIND_STMT: case NODE_MUT_BIND_STMT: {
        Type *ann =(node->child_count>1&&node->children[1])?resolve_type(cx,node->children[1]):NULL;
        Type *init=(node->child_count>2&&node->children[2])?infer(cx,node->children[2]):NULL;
        if (ann&&init&&ann->kind!=TY_UNKNOWN&&init->kind!=TY_UNKNOWN&&!types_equal(ann,init)) {
            char fix[128]; snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(ann));
            emit_mm(cx,node,ann,init,fix);
        }
        return mk_type(A,TY_VOID);
    }

    case NODE_ASSIGN_STMT: {
        Type *lhs=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_UNKNOWN);
        Type *rhs=node->child_count>1?infer(cx,node->children[1]):mk_type(A,TY_UNKNOWN);
        if (lhs->kind!=TY_UNKNOWN&&rhs->kind!=TY_UNKNOWN&&!types_equal(lhs,rhs)) {
            char fix[128]; snprintf(fix,sizeof(fix),"cast RHS to %s using 'as'",type_name(lhs));
            emit_mm(cx,node,lhs,rhs,fix);
        }
        return mk_type(A,TY_VOID);
    }

    case NODE_RETURN_STMT: {
        Type *val=node->child_count>0?infer(cx,node->children[0]):mk_type(A,TY_VOID);
        if (cx->fn_ret&&cx->fn_ret->kind!=TY_UNKNOWN&&val->kind!=TY_UNKNOWN
            &&!types_equal(cx->fn_ret,val)) {
            char fix[128]; snprintf(fix,sizeof(fix),"cast return value to %s using 'as'",type_name(cx->fn_ret));
            emit_mm(cx,node,cx->fn_ret,val,fix);
        }
        return mk_type(A,TY_VOID);
    }

    case NODE_MATCH_STMT: {
        if (node->child_count<1) return mk_type(A,TY_VOID);
        Type *scr=infer(cx,node->children[0]),*arm_type=NULL; int has_t=0,has_f=0;
        for (int i=1;i<node->child_count;i++) {
            const Node *arm=node->children[i];
            if (!arm||arm->kind!=NODE_MATCH_ARM) continue;
            if (scr->kind==TY_BOOL&&arm->child_count>0&&arm->children[0]&&arm->children[0]->kind==NODE_BOOL_LIT) {
                char pb[8]; TOKSTR(pb,cx->src,arm->children[0]);
                if (strcmp(pb,"true")==0) has_t=1; else has_f=1;
            }
            Type *at=arm->child_count>1&&arm->children[1]?infer(cx,arm->children[1]):mk_type(A,TY_VOID);
            if (!arm_type) { arm_type=at; }
            else if (at->kind!=TY_UNKNOWN&&arm_type->kind!=TY_UNKNOWN&&!types_equal(arm_type,at)) {
                char msg[256];
                snprintf(msg,sizeof(msg),"match arms have inconsistent types: '%s' vs '%s'",type_name(arm_type),type_name(at));
                diag_emit(DIAG_ERROR,E4011,arm->start,arm->line,arm->col,msg,"fix",(const char*)NULL);
                cx->had_error=1;
            }
        }
        if (scr->kind==TY_BOOL&&!has_t) { diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,
            "non-exhaustive match: missing arm for 'true'","fix",(const char*)NULL); cx->had_error=1; }
        if (scr->kind==TY_BOOL&&!has_f) { diag_emit(DIAG_ERROR,E4010,node->start,node->line,node->col,
            "non-exhaustive match: missing arm for 'false'","fix",(const char*)NULL); cx->had_error=1; }
        return arm_type?arm_type:mk_type(A,TY_VOID);
    }

    case NODE_ARENA_STMT: {
        cx->env->arena_depth++;
        for (int i=0;i<node->child_count;i++) {
            const Node *ch=node->children[i];
            if (ch&&ch->kind==NODE_ASSIGN_STMT&&ch->child_count>0) {
                const Node *ln=ch->children[0];
                if (ln&&ln->kind==NODE_IDENT) {
                    char nb[128]; TOKSTR(nb,cx->src,ln);
                    Decl *d=tc_lookup(cx->env->names->module_scope,nb,(int)strlen(nb));
                    if (d&&d->def_node) {
                        diag_emit(DIAG_ERROR,E5001,ch->start,ch->line,ch->col,
                            "value escapes arena scope: cannot assign arena-allocated value to outer variable",
                            "fix",(const char*)NULL);
                        cx->had_error=1;
                    }
                }
            }
            infer(cx,ch);
        }
        cx->env->arena_depth--;
        return mk_type(A,TY_VOID);
    }

    case NODE_FUNC_DECL: {
        Type *ret=mk_type(A,TY_VOID);
        for (int i=0;i<node->child_count;i++) {
            const Node *ch=node->children[i];
            if (ch&&ch->kind==NODE_RETURN_SPEC&&ch->child_count>0)
                { ret=resolve_type(cx,ch->children[0]); break; }
        }
        Type *saved=cx->fn_ret; cx->fn_ret=ret;
        for (int i=0;i<node->child_count;i++) infer(cx,node->children[i]);
        cx->fn_ret=saved;
        return mk_type(A,TY_VOID);
    }

    default: {
        Type *last=mk_type(A,TY_VOID);
        for (int i=0;i<node->child_count;i++) last=infer(cx,node->children[i]);
        return last;
    }
    }
}

int type_check(const Node *ast, const char *src,
               NameEnv *names, Arena *arena, TypeEnv *out) {
    if (!ast||!src||!names||!arena||!out) return -1;
    out->names=names; out->arena=arena; out->arena_depth=0;
    Ctx cx; cx.env=out; cx.src=src; cx.fn_ret=NULL; cx.had_error=0;
    for (int i=0;i<ast->child_count;i++) infer(&cx,ast->children[i]);
    return cx.had_error?-1:0;
}
