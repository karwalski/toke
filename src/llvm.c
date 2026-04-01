/* llvm.c — LLVM IR text emitter for toke. Story: 1.2.8 */
#include "llvm.h"
#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_FUNCS 128
#define MAX_PTR_LOCALS 64
#define MAX_STRUCT_TYPES 64
typedef struct { char name[128]; const char *ret; char ret_type_name[128]; const char *param_tys[NODE_MAX_CHILDREN]; char param_type_names[NODE_MAX_CHILDREN][128]; int param_count; } FnSig;
typedef struct { char name[128]; char struct_type[128]; } PtrLocal;
typedef struct { char name[128]; int field_count; char field_names[NODE_MAX_CHILDREN][128]; } StructInfo;
typedef struct { FILE *out; const char *src; int tmp, str_idx, lbl; int term; FnSig fns[MAX_FUNCS]; int fn_count; PtrLocal ptrs[MAX_PTR_LOCALS]; int ptr_count; StructInfo structs[MAX_STRUCT_TYPES]; int struct_count; const char *cur_fn_ret; } Ctx;
static int next_tmp(Ctx *c){return c->tmp++;} static int next_lbl(Ctx *c){return c->lbl++;} static int next_str(Ctx *c){return c->str_idx++;}
static void tok_cp(const char *src,const Node *n,char *buf,int sz){int len=n->tok_len<sz-1?n->tok_len:sz-1;memcpy(buf,src+n->tok_start,(size_t)len);buf[len]='\0';}

static void mark_ptr_with_type(Ctx *c, const char *name, const char *stype) {
    if (c->ptr_count >= MAX_PTR_LOCALS) return;
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(c->ptrs[c->ptr_count].name, name, (size_t)len);
    c->ptrs[c->ptr_count].name[len] = '\0';
    c->ptrs[c->ptr_count].struct_type[0] = '\0';
    if (stype) {
        int slen = (int)strlen(stype);
        if (slen >= 128) slen = 127;
        memcpy(c->ptrs[c->ptr_count].struct_type, stype, (size_t)slen);
        c->ptrs[c->ptr_count].struct_type[slen] = '\0';
    }
    c->ptr_count++;
}

static int is_ptr_local(Ctx *c, const char *name) {
    for (int i = 0; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, name)) return 1;
    return 0;
}
static const char *ptr_local_struct_type(Ctx *c, const char *name) {
    for (int i = 0; i < c->ptr_count; i++)
        if (!strcmp(c->ptrs[i].name, name) && c->ptrs[i].struct_type[0])
            return c->ptrs[i].struct_type;
    return NULL;
}

/* ── Struct type registry ──────────────────────────────────────────── */

static void register_struct(Ctx *c, const char *name, int fc, const Node *decl, const char *src) {
    if (c->struct_count >= MAX_STRUCT_TYPES) return;
    StructInfo *si = &c->structs[c->struct_count];
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(si->name, name, (size_t)len);
    si->name[len] = '\0';
    si->field_count = fc;
    /* Extract field names from type decl.  Fields may be direct children
     * or wrapped in a NODE_STMT_LIST (from parse_field_list). */
    int fi = 0;
    for (int i = 1; i < decl->child_count && fi < NODE_MAX_CHILDREN; i++) {
        const Node *ch = decl->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_FIELD) {
            tok_cp(src, ch, si->field_names[fi++], 128);
        } else if (ch->kind == NODE_STMT_LIST) {
            for (int j = 0; j < ch->child_count && fi < NODE_MAX_CHILDREN; j++) {
                const Node *fj = ch->children[j];
                if (fj && fj->kind == NODE_FIELD)
                    tok_cp(src, fj, si->field_names[fi++], 128);
            }
        }
    }
    c->struct_count++;
}

static const StructInfo *lookup_struct(Ctx *c, const char *name) {
    for (int i = 0; i < c->struct_count; i++)
        if (!strcmp(c->structs[i].name, name)) return &c->structs[i];
    return NULL;
}

/* Return the field index for a given field name in a struct, or 0 if not found. */
static int struct_field_index(const StructInfo *si, const char *fname) {
    if (!si) return 0;
    for (int i = 0; i < si->field_count; i++)
        if (!strcmp(si->field_names[i], fname)) return i;
    return 0; /* fallback */
}

static int is_struct_type_name(Ctx *c, const char *name) {
    return lookup_struct(c, name) != NULL;
}

/* Return 1 if a type-expression AST node represents a pointer-at-LLVM-level type
 * (arrays, maps, pointers, strings, structs). */
static int is_ptr_type_node(Ctx *c, const Node *ty) {
    if (!ty) return 0;
    if (ty->kind == NODE_ARRAY_TYPE || ty->kind == NODE_MAP_TYPE || ty->kind == NODE_PTR_TYPE)
        return 1;
    /* TYPE_IDENT that names a struct type */
    if (ty->kind == NODE_TYPE_IDENT || ty->kind == NODE_TYPE_EXPR) {
        char tn[128]; tok_cp(c->src, ty, tn, sizeof tn);
        if (is_struct_type_name(c, tn)) return 1;
    }
    return 0;
}

/* Resolve a return-spec or param type node to an LLVM type string. */
static const char *resolve_llvm_type(Ctx *c, const Node *ty) {
    if (!ty) return "i64";
    if (is_ptr_type_node(c, ty)) return "ptr";
    char tn[64]; tok_cp(c->src, ty, tn, sizeof tn);
    if (!strcmp(tn, "bool")) return "i1";
    if (!strcmp(tn, "f64"))  return "double";
    if (!strcmp(tn, "str"))  return "ptr";
    if (!strcmp(tn, "void")) return "void";
    return "i64";
}

static FnSig *register_fn(Ctx *c, const char *name, const char *ret) {
    if (c->fn_count >= MAX_FUNCS) return NULL;
    FnSig *s = &c->fns[c->fn_count];
    int len = (int)strlen(name);
    if (len >= 128) len = 127;
    memcpy(s->name, name, (size_t)len);
    s->name[len] = '\0';
    s->ret = ret;
    s->ret_type_name[0] = '\0';
    s->param_count = 0;
    c->fn_count++;
    return s;
}
static const FnSig *lookup_fn(Ctx *c, const char *name) {
    for (int i = 0; i < c->fn_count; i++)
        if (!strcmp(c->fns[i].name, name)) return &c->fns[i];
    return NULL;
}

/* ── Prepass: collect struct type declarations ─────────────────────── */

static void prepass_structs(Ctx *c, const Node *n) {
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_structs(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_TYPE_DECL || n->child_count < 1) return;
    char tb[128]; tok_cp(c->src, n->children[0], tb, sizeof tb);
    /* Count fields: they may be in a NODE_STMT_LIST child */
    int fc = 0;
    for (int i = 1; i < n->child_count; i++) {
        const Node *ch = n->children[i];
        if (!ch) continue;
        if (ch->kind == NODE_FIELD) fc++;
        else if (ch->kind == NODE_STMT_LIST)
            for (int j = 0; j < ch->child_count; j++)
                if (ch->children[j] && ch->children[j]->kind == NODE_FIELD) fc++;
    }
    register_struct(c, tb, fc, n, c->src);
}

static void prepass_funcs(Ctx *c, const Node *n) {
    char tb[256];
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_funcs(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_FUNC_DECL) return;
    tok_cp(c->src, n->children[0], tb, sizeof tb);
    const char *ret = "void";
    char ret_tn[128] = "";
    for (int i = 1; i < n->child_count; i++) {
        if (n->children[i]->kind == NODE_RETURN_SPEC) {
            const Node *rs = n->children[i];
            if (rs->child_count > 0) {
                ret = resolve_llvm_type(c, rs->children[0]);
                tok_cp(c->src, rs->children[0], ret_tn, sizeof ret_tn);
            }
        }
    }
    FnSig *sig = register_fn(c, tb, ret);
    if (sig) {
        memcpy(sig->ret_type_name, ret_tn, sizeof sig->ret_type_name);
        for (int i = 1; i < n->child_count && sig->param_count < NODE_MAX_CHILDREN; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1]) {
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
                tok_cp(c->src, n->children[i]->children[1],
                       sig->param_type_names[sig->param_count], 128);
            } else {
                sig->param_type_names[sig->param_count][0] = '\0';
            }
            sig->param_tys[sig->param_count++] = pty;
        }
    }
}

static int emit_str_global(Ctx *c, const char *raw, int rlen)
{
    const char *inner = raw + 1; int ilen = rlen-2; if(ilen<0)ilen=0;
    int idx = next_str(c);
    fprintf(c->out,"@.str.%d = private unnamed_addr constant [%d x i8] c\"",idx,ilen+1);
    for(int i=0;i<ilen;i++){
        unsigned char ch=(unsigned char)inner[i];
        if(ch=='\\'&&i+1<ilen){char nx=inner[i+1];
            if(nx=='n'){fputs("\\0A",c->out);i++;continue;}
            if(nx=='t'){fputs("\\09",c->out);i++;continue;}
            if(nx=='\\'){fputs("\\5C",c->out);i++;continue;}
            if(nx=='"'){fputs("\\22",c->out);i++;continue;}
        }
        if(ch>=32&&ch<127&&ch!='"'&&ch!='\\')fputc((char)ch,c->out);
        else
            fprintf(c->out, "\\%02X", ch);
    }
    fputs("\\00\"\n", c->out);
    return idx;
}

/* ── Expression emission ───────────────────────────────────────────── */

static int emit_expr(Ctx *c, const Node *n);
static void emit_stmt(Ctx *c, const Node *n);

/* Resolve the struct type name from a NODE_FIELD_EXPR's base expression.
 * Walks through identifiers to find the struct type.  Returns NULL if unknown. */
static const StructInfo *resolve_base_struct(Ctx *c, const Node *base) {
    /* For a NODE_STRUCT_LIT, the type name is in its token */
    if (base->kind == NODE_STRUCT_LIT) {
        char tn[128]; tok_cp(c->src, base, tn, sizeof tn);
        return lookup_struct(c, tn);
    }
    /* For a NODE_IDENT, check if it's a ptr-local with a known struct type */
    if (base->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, base, nb, sizeof nb);
        const char *stype = ptr_local_struct_type(c, nb);
        if (stype) return lookup_struct(c, stype);
    }
    return NULL;
}

static int emit_expr(Ctx *c, const Node *n)
{
    char tb[256]; int t, t2, t3;
    switch (n->kind) {
    case NODE_INT_LIT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i64 0, %s\n", t, tb);
        return t;
    case NODE_FLOAT_LIT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = fadd double 0.0, %s\n", t, tb);
        return t;
    case NODE_BOOL_LIT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i1 0, %d\n", t, tb[0]=='t' ? 1 : 0);
        return t;
    case NODE_STR_LIT: {
        int alen = n->tok_len - 2 + 1; if (alen < 1) alen = 1;
        int si = emit_str_global(c, c->src + n->tok_start, n->tok_len);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr inbounds [%d x i8], ptr @.str.%d, i32 0, i32 0\n",
                t, alen, si);
        return t;
    }
    case NODE_IDENT:
        tok_cp(c->src, n, tb, sizeof tb);
        t = next_tmp(c);
        if (is_ptr_local(c, tb))
            fprintf(c->out, "  %%t%d = load ptr, ptr %%%s\n", t, tb);
        else
            fprintf(c->out, "  %%t%d = load i64, ptr %%%s\n", t, tb);
        return t;
    case NODE_BINARY_EXPR: {
        int lhs=emit_expr(c,n->children[0]), rhs=emit_expr(c,n->children[1]);
        t = next_tmp(c);
        const char *op;
        switch (n->op) {
        case TK_PLUS:  op = "add i64";      break;
        case TK_MINUS: op = "sub i64";      break;
        case TK_STAR:  op = "mul i64";      break;
        case TK_SLASH: op = "sdiv i64";     break;
        case TK_LT:    op = "icmp slt i64"; break;
        case TK_GT:    op = "icmp sgt i64"; break;
        case TK_EQ:    op = "icmp eq i64";  break;
        default:       op = "add i64";
            fprintf(c->out, "  ; unsupported binop %d\n", (int)n->op);
        }
        fprintf(c->out, "  %%t%d = %s %%t%d, %%t%d\n", t, op, lhs, rhs);
        return t;
    }
    case NODE_UNARY_EXPR: {
        int v = emit_expr(c, n->children[0]);
        t = next_tmp(c);
        if (n->op == TK_MINUS)
            fprintf(c->out, "  %%t%d = sub i64 0, %%t%d\n", t, v);
        else if (n->op == TK_BANG)
            fprintf(c->out, "  %%t%d = xor i1 %%t%d, 1\n", t, v);
        else
            fprintf(c->out, "  %%t%d = add i64 0, %%t%d ; unary stub\n", t, v);
        return t;
    }
    case NODE_CALL_EXPR: {
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        /* spawn(fn) -> call ptr @tk_spawn(ptr @fn) */
        if (!strcmp(tb, "spawn") && n->child_count >= 2) {
            char fn_name[256];
            tok_cp(c->src, n->children[1], fn_name, sizeof fn_name);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call ptr @tk_spawn(ptr @%s)\n", t, fn_name);
            return t;
        }
        /* await(t) -> call i64 @tk_await(ptr %t) */
        if (!strcmp(tb, "await") && n->child_count >= 2) {
            int av = emit_expr(c, n->children[1]);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call i64 @tk_await(ptr %%t%d)\n", t, av);
            return t;
        }
        int args[NODE_MAX_CHILDREN], na = n->child_count - 1;
        for (int i = 0; i < na; i++) args[i] = emit_expr(c, n->children[i+1]);
        const FnSig *callee = lookup_fn(c, tb);
        const char *callee_ret = callee ? callee->ret : "i64";
        if (!strcmp(callee_ret, "void")) {
            fprintf(c->out, "  call void @%s(", tb);
            for (int i = 0; i < na; i++) {
                if (i) fputc(',', c->out);
                const char *aty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
                fprintf(c->out, " %s %%t%d", aty, args[i]);
            }
            fputs(")\n", c->out);
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
            return t;
        }
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call %s @%s(", t, callee_ret, tb);
        for (int i = 0; i < na; i++) {
            if (i) fputc(',', c->out);
            const char *aty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
            fprintf(c->out, " %s %%t%d", aty, args[i]);
        }
        fputs(")\n", c->out);
        return t;
    }
    case NODE_CAST_EXPR: {
        int v = emit_expr(c, n->children[0]);
        t = next_tmp(c);
        if (n->child_count >= 2 && n->children[1]) {
            char tn[64]; tok_cp(c->src, n->children[1], tn, sizeof tn);
            if (!strcmp(tn, "f64")) {
                fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", t, v);
                return t;
            } else if (!strcmp(tn, "i64")) {
                fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", t, v);
                return t;
            } else if (!strcmp(tn, "bool")) {
                fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", t, v);
                return t;
            }
        }
        /* identity / unknown cast */
        fprintf(c->out, "  %%t%d = add i64 0, %%t%d ; as-cast identity\n", t, v);
        return t;
    }
    case NODE_FIELD_EXPR: {
        /* Determine the struct type from the base expression so we can
         * compute the correct field index for the GEP. */
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        int base = emit_expr(c, n->children[0]);
        /* Try to find the struct info for the base.  For identifiers we
         * do not yet track their type, so fall back to looking at the
         * node's struct literal if present, or use index 0 by default. */
        int fidx = 0;
        const StructInfo *si = resolve_base_struct(c, n->children[0]);
        if (si) fidx = struct_field_index(si, fn);
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i32 %d ; .%s\n", t2, base, fidx, fn);
        fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
        return t;
    }
    case NODE_INDEX_EXPR: {
        int base = emit_expr(c, n->children[0]);
        int idx  = emit_expr(c, n->children[1]);
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i64 %%t%d\n", t2, base, idx);
        fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
        return t;
    }
    case NODE_STRUCT_LIT: {
        /* Look up the struct to get field count and field names for
         * correct allocation and field-to-index mapping. */
        char sn[128]; tok_cp(c->src, n, sn, sizeof sn);
        const StructInfo *si = lookup_struct(c, sn);
        int nfields = si ? si->field_count : (n->child_count > 0 ? n->child_count : 1);
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = alloca i64, i32 %d ; struct_lit %s\n", t, nfields, sn);
        for (int i = 0; i < n->child_count; i++) {
            const Node *fi = n->children[i];
            if (!fi || fi->kind != NODE_FIELD_INIT) continue;
            /* Determine field index: match the field name from the init
             * against the struct declaration's field order. */
            int fidx = i; /* default: positional */
            if (si && fi->tok_len > 0) {
                char fname[128]; tok_cp(c->src, fi, fname, sizeof fname);
                fidx = struct_field_index(si, fname);
            }
            if (fi->child_count >= 1) {
                t3 = emit_expr(c, fi->children[0]);
                t2 = next_tmp(c);
                fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i32 %d ; .%s\n",
                        t2, t, fidx, si ? si->field_names[fidx] : "?");
                fprintf(c->out, "  store i64 %%t%d, ptr %%t%d\n", t3, t2);
            }
        }
        return t;
    }
    case NODE_ARRAY_LIT: {
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = alloca i64, i64 %d\n", t, n->child_count);
        for (int i = 0; i < n->child_count; i++) {
            t2 = emit_expr(c, n->children[i]);
            t3 = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i64 %d\n", t3, t, i);
            fprintf(c->out, "  store i64 %%t%d, ptr %%t%d\n", t2, t3);
        }
        return t;
    }
    case NODE_MAP_LIT: {
        /* Emit calls to tk_map_new and tk_map_put — runtime stubs. */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call ptr @tk_map_new()\n", t);
        for (int i = 0; i < n->child_count; i++) {
            const Node *entry = n->children[i];
            if (entry->child_count >= 2) {
                int kv = emit_expr(c, entry->children[0]);
                int vv = emit_expr(c, entry->children[1]);
                fprintf(c->out, "  call void @tk_map_put(ptr %%t%d, i64 %%t%d, i64 %%t%d)\n", t, kv, vv);
            }
        }
        return t;
    }
    case NODE_PROPAGATE_EXPR:
        fprintf(c->out, "  ; propagate expr stub\n");
        return emit_expr(c, n->children[0]);
    default:
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = add i64 0, 0 ; unhandled expr %d\n", t, (int)n->kind);
        return t;
    }
}

/* Return the struct type name of an expression, or NULL if not a struct. */
static const char *expr_struct_type(Ctx *c, const Node *n) {
    if (!n) return NULL;
    if (n->kind == NODE_STRUCT_LIT) {
        /* For struct literals, the type name is in the token */
        static char sn[128];
        tok_cp(c->src, n, sn, sizeof sn);
        if (lookup_struct(c, sn)) return sn;
        return NULL;
    }
    if (n->kind == NODE_IDENT) {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        return ptr_local_struct_type(c, nb);
    }
    if (n->kind == NODE_CALL_EXPR && n->child_count >= 1) {
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig && sig->ret_type_name[0] && lookup_struct(c, sig->ret_type_name))
            return sig->ret_type_name;
        return NULL;
    }
    return NULL;
}

/* Return 1 if the expression node will produce a ptr-typed LLVM value. */
static int expr_is_ptr(Ctx *c, const Node *n) {
    if (!n) return 0;
    switch (n->kind) {
    case NODE_STRUCT_LIT:
    case NODE_ARRAY_LIT:
    case NODE_MAP_LIT:
        return 1;
    case NODE_IDENT: {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        return is_ptr_local(c, nb);
    }
    case NODE_CALL_EXPR: {
        if (n->child_count < 1) return 0;
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        return sig && !strcmp(sig->ret, "ptr");
    }
    default:
        return 0;
    }
}

/* ── Statement emission ────────────────────────────────────────────── */

static void emit_stmt(Ctx *c, const Node *n)
{
    char tb[256];
    switch (n->kind) {
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        if (n->child_count >= 2 && expr_is_ptr(c, n->children[1])) {
            const char *stype = expr_struct_type(c, n->children[1]);
            mark_ptr_with_type(c, tb, stype);
            fprintf(c->out, "  %%%s = alloca ptr\n", tb);
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store ptr %%t%d, ptr %%%s\n", v, tb);
        } else {
            fprintf(c->out, "  %%%s = alloca i64\n", tb);
            if (n->child_count >= 2) {
                int v = emit_expr(c, n->children[1]);
                fprintf(c->out, "  store i64 %%t%d, ptr %%%s\n", v, tb);
            }
        }
        break;
    case NODE_ASSIGN_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        if (is_ptr_local(c, tb)) {
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store ptr %%t%d, ptr %%%s\n", v, tb);
        } else {
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store i64 %%t%d, ptr %%%s\n", v, tb);
        }
        break;
    case NODE_RETURN_STMT:
        if (n->child_count > 0) {
            int v = emit_expr(c, n->children[0]);
            /* Use the function's return type for the ret instruction */
            const char *rt = c->cur_fn_ret ? c->cur_fn_ret : "i64";
            fprintf(c->out, "  ret %s %%t%d\n", rt, v);
        } else {
            fputs("  ret void\n", c->out);
        }
        c->term = 1;
        break;
    case NODE_BREAK_STMT:
        fprintf(c->out, "  br label %%loop_exit%d\n", c->lbl - 1);
        c->term = 1;
        break;
    case NODE_IF_STMT: {
        int L = next_lbl(c);
        int cv = emit_expr(c, n->children[0]);
        fprintf(c->out, "  br i1 %%t%d, label %%if_then%d, label %%if_else%d\n", cv, L, L);
        fprintf(c->out, "if_then%d:\n", L);
        c->term = 0;
        emit_stmt(c, n->children[1]);
        int then_term = c->term;
        if (!then_term) fprintf(c->out, "  br label %%if_merge%d\n", L);
        fprintf(c->out, "if_else%d:\n", L);
        c->term = 0;
        if (n->child_count >= 3) emit_stmt(c, n->children[2]);
        int else_term = c->term;
        if (!else_term) fprintf(c->out, "  br label %%if_merge%d\n", L);
        if (!then_term || !else_term) fprintf(c->out, "if_merge%d:\n", L);
        c->term = then_term && else_term;
        break;
    }
    case NODE_LOOP_STMT: {
        int L = next_lbl(c);
        /* optional init */
        if (n->child_count > 0 && n->children[0] && n->children[0]->kind == NODE_LOOP_INIT)
            emit_stmt(c, n->children[0]);
        fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_hdr%d:\n", L);
        /* body is always last child */
        const Node *body = n->children[n->child_count - 1];
        /* optional condition: second child if it's not the body and not init */
        if (n->child_count >= 2) {
            const Node *maybe_cond = n->children[n->child_count == 2 ? 0 : 1];
            if (maybe_cond->kind != NODE_LOOP_INIT && maybe_cond->kind != NODE_STMT_LIST) {
                int cv = emit_expr(c, maybe_cond);
                fprintf(c->out, "  br i1 %%t%d, label %%loop_body%d, label %%loop_exit%d\n", cv, L, L);
            } else {
                fprintf(c->out, "  br label %%loop_body%d\n", L);
            }
        } else {
            fprintf(c->out, "  br label %%loop_body%d\n", L);
        }
        fprintf(c->out, "loop_body%d:\n", L);
        int save = c->lbl; c->lbl = L + 1;
        c->term = 0;
        emit_stmt(c, body);
        c->lbl = save;
        if (!c->term) fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_exit%d:\n", L);
        c->term = 0;
        break;
    }
    case NODE_ARENA_STMT:
        fputs("  ; arena begin\n", c->out);
        if (n->child_count > 0) emit_stmt(c, n->children[0]);
        fputs("  ; arena end\n", c->out);
        break;
    case NODE_MATCH_STMT: {
        int ML = next_lbl(c);
        int sv = emit_expr(c, n->children[0]);
        for (int i = 1; i < n->child_count; i++) {
            const Node *arm = n->children[i];
            int AL = next_lbl(c);
            int pv = emit_expr(c, arm->children[0]);
            int cv = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp eq i64 %%t%d, %%t%d\n", cv, sv, pv);
            fprintf(c->out, "  br i1 %%t%d, label %%marm%d, label %%mnxt%d\n", cv, AL, AL);
            fprintf(c->out, "marm%d:\n", AL);
            if (arm->child_count >= 3) emit_stmt(c, arm->children[2]);
            else if (arm->child_count >= 2) emit_stmt(c, arm->children[1]);
            fprintf(c->out, "  br label %%mend%d\n", ML);
            fprintf(c->out, "mnxt%d:\n", AL);
        }
        fprintf(c->out, "mend%d:\n", ML);
        break;
    }
    case NODE_STMT_LIST:
        for (int i = 0; i < n->child_count; i++) emit_stmt(c, n->children[i]);
        break;
    case NODE_EXPR_STMT:
        if (n->child_count > 0) (void)emit_expr(c, n->children[0]);
        break;
    default:
        fprintf(c->out, "  ; unhandled stmt %d\n", (int)n->kind);
        break;
    }
}

/* ── Top-level declarations ────────────────────────────────────────── */

static void emit_toplevel(Ctx *c, const Node *n)
{
    char tb[256];
    switch (n->kind) {
    case NODE_PROGRAM:
    case NODE_MODULE:
        for (int i = 0; i < n->child_count; i++) emit_toplevel(c, n->children[i]);
        break;
    case NODE_TYPE_DECL:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        fprintf(c->out, "%%struct.%s = type { ", tb);
        { int f = n->child_count - 1;
          for (int i = 0; i < f; i++) { if (i) fputs(", ", c->out); fputs("i64", c->out); }
          if (f == 0) fputs("i8", c->out); }
        fputs(" }\n", c->out);
        break;
    case NODE_CONST_DECL:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        if (n->child_count >= 3 && n->children[2]->kind == NODE_INT_LIT) {
            char vb[64]; tok_cp(c->src, n->children[2], vb, sizeof vb);
            fprintf(c->out, "@%s = constant i64 %s\n", tb, vb);
        } else if (n->child_count >= 3 && n->children[2]->kind == NODE_STR_LIT) {
            const Node *sl = n->children[2];
            int ilen = sl->tok_len - 2 + 1; if (ilen < 1) ilen = 1;
            int si = emit_str_global(c, c->src + sl->tok_start, sl->tok_len);
            fprintf(c->out, "@%s = constant ptr getelementptr ([%d x i8], ptr @.str.%d, i32 0, i32 0)\n",
                    tb, ilen, si);
        } else {
            fprintf(c->out, "@%s = constant i64 0 ; const stub\n", tb);
        }
        break;
    case NODE_FUNC_DECL: {
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        /* Determine return type from NODE_RETURN_SPEC if present */
        const char *ret = "void";
        int body_i = -1;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind == NODE_STMT_LIST)  body_i = i;
            if (n->children[i]->kind == NODE_RETURN_SPEC) {
                const Node *rs = n->children[i];
                if (rs->child_count > 0)
                    ret = resolve_llvm_type(c, rs->children[0]);
            }
        }
        /* Map parameter types to LLVM types */
        if (body_i < 0) {
            /* extern: emit declare */
            fprintf(c->out, "\ndeclare %s @%s(", ret, tb);
            int first = 1;
            for (int i = 1; i < n->child_count; i++) {
                if (n->children[i]->kind != NODE_PARAM) continue;
                if (!first) fputs(", ", c->out);
                const char *pty = "i64";
                if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                    pty = resolve_llvm_type(c, n->children[i]->children[1]);
                fputs(pty, c->out);
                first = 0;
            }
            fputs(")\n", c->out);
            break;
        }
        c->ptr_count = 0; /* reset ptr-local tracking for each function */
        c->cur_fn_ret = ret;
        fprintf(c->out, "\ndefine %s @%s(", ret, tb);
        int first = 1;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            if (!first) fputs(", ", c->out);
            int pptr = (n->children[i]->child_count > 1 && is_ptr_type_node(c, n->children[i]->children[1]));
            fprintf(c->out, "%s %%%s.arg", pptr ? "ptr" : "i64", pn);
            first = 0;
        }
        fputs(") {\nentry:\n", c->out);
        /* Spill params */
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            int pptr = (n->children[i]->child_count > 1 && is_ptr_type_node(c, n->children[i]->children[1]));
            if (pptr) {
                /* Extract the type name for struct tracking */
                char pty_name[128] = "";
                if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                    tok_cp(c->src, n->children[i]->children[1], pty_name, sizeof pty_name);
                const char *stype = lookup_struct(c, pty_name) ? pty_name : NULL;
                mark_ptr_with_type(c, pn, stype);
                fprintf(c->out, "  %%%s = alloca ptr\n  store ptr %%%s.arg, ptr %%%s\n", pn, pn, pn);
            } else {
                fprintf(c->out, "  %%%s = alloca i64\n  store i64 %%%s.arg, ptr %%%s\n", pn, pn, pn);
            }
        }
        c->term = 0;
        if (body_i >= 0) emit_stmt(c, n->children[body_i]);
        if (!c->term) {
            if (!strcmp(ret, "void")) fputs("  ret void\n", c->out);
            else fprintf(c->out, "  ret %s 0 ; implicit return\n", ret);
        }
        fputs("}\n", c->out);
        c->cur_fn_ret = NULL;
        break;
    }
    case NODE_IMPORT: break;  /* nothing to emit */
    default:
        fprintf(c->out, "; skipped top-level kind %d\n", (int)n->kind);
        break;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

int emit_llvm_ir(const Node *ast, const char *src,
                 const CodegenEnv *env, const char *out_ll)
{
    FILE *f = fopen(out_ll, "w");
    if (!f) {
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: cannot open output file", (void*)0);
        return -1;
    }
    Ctx ctx; memset(&ctx, 0, sizeof ctx); ctx.out = f; ctx.src = src;

    fputs("; module generated by tkc\n", f);
    if (env && env->target && env->target[0])
        fprintf(f, "target triple = \"%s\"\n", env->target);
    fputs("\ndeclare i32 @printf(ptr, ...)\ndeclare i32 @puts(ptr)\n", f);
    fputs("declare ptr @tk_spawn(ptr)\ndeclare i64 @tk_await(ptr)\n\n", f);

    prepass_structs(&ctx, ast);
    prepass_funcs(&ctx, ast);
    emit_toplevel(&ctx, ast);

    if (fflush(f) || ferror(f)) {
        fclose(f);
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: I/O error writing .ll file", (void*)0);
        return -1;
    }
    fclose(f);
    return 0;
}

int compile_binary(const char *out_ll, const char *out_bin, const char *target)
{
    char cmd[1024];
    if(target&&target[0]) snprintf(cmd,sizeof cmd,"clang -O1 -target %s -o %s %s",target,out_bin,out_ll);
    else                  snprintf(cmd,sizeof cmd,"clang -O1 -o %s %s",out_bin,out_ll);
    int rc = system(cmd);
    if(rc!=0){char msg[256];snprintf(msg,sizeof msg,"clang invocation failed with exit code %d",rc);
        diag_emit(DIAG_ERROR,E9003,0,0,0,msg,(void*)0);return -1;}
    return 0;
}
