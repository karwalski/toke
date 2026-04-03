/* llvm.c — LLVM IR text emitter for toke. Story: 1.2.8 */
#include "llvm.h"
#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define MAX_FUNCS 128
#define MAX_PTR_LOCALS 64
#define MAX_STRUCT_TYPES 64
#define MAX_IMPORTS 32
typedef struct { char name[128]; const char *ret; char ret_type_name[128]; const char *param_tys[NODE_MAX_CHILDREN]; char param_type_names[NODE_MAX_CHILDREN][128]; int param_count; } FnSig;
typedef struct { char name[128]; char struct_type[128]; } PtrLocal;
typedef struct { char name[128]; int field_count; char field_names[NODE_MAX_CHILDREN][128]; } StructInfo;
typedef struct { char alias[64]; char module[64]; } ImportAlias; /* e.g. alias="j", module="json" */
#define MAX_LOCALS 128
typedef struct { char name[128]; const char *ty; } LocalType;
typedef struct { char toke_name[128]; char llvm_name[128]; } NameAlias;
typedef struct { FILE *out; const char *src; int tmp, str_idx, lbl; int term; int break_lbl; FnSig fns[MAX_FUNCS]; int fn_count; PtrLocal ptrs[MAX_PTR_LOCALS]; int ptr_count; StructInfo structs[MAX_STRUCT_TYPES]; int struct_count; const char *cur_fn_ret; ImportAlias imports[MAX_IMPORTS]; int import_count; LocalType locals[MAX_LOCALS]; int local_count; NameAlias aliases[MAX_LOCALS]; int alias_count; int name_scope; char str_globals[64*1024]; int str_globals_len; } Ctx;
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
    if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
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

/* ── Import alias collection ──────────────────────────────────────── */

static void prepass_imports(Ctx *c, const Node *n) {
    if (n->kind == NODE_PROGRAM || n->kind == NODE_MODULE) {
        for (int i = 0; i < n->child_count; i++) prepass_imports(c, n->children[i]);
        return;
    }
    if (n->kind != NODE_IMPORT || c->import_count >= MAX_IMPORTS) return;
    /* NODE_IMPORT children: [0]=alias (IDENT), [1..]=module path segments (IDENT) */
    if (n->child_count < 2) return;
    ImportAlias *ia = &c->imports[c->import_count];
    tok_cp(c->src, n->children[0], ia->alias, sizeof ia->alias);
    /* Module path: children[1] is NODE_MODULE_PATH with IDENT children.
     * Last IDENT child is the module name (e.g. std.json → "json") */
    const Node *mp = n->children[1];
    if (mp->kind == NODE_MODULE_PATH && mp->child_count > 0)
        tok_cp(c->src, mp->children[mp->child_count - 1], ia->module, sizeof ia->module);
    else
        tok_cp(c->src, mp, ia->module, sizeof ia->module);
    c->import_count++;
}

/* Resolve a qualified call like j.parse(x) to a C runtime function name.
 * Returns the C function name, or NULL if not a stdlib call. */
static const char *resolve_stdlib_call(Ctx *c, const char *alias, const char *method) {
    /* Find which module this alias refers to */
    const char *mod = NULL;
    for (int i = 0; i < c->import_count; i++) {
        if (!strcmp(c->imports[i].alias, alias)) { mod = c->imports[i].module; break; }
    }
    if (!mod) return NULL;

    /* std.json functions */
    if (!strcmp(mod, "json")) {
        if (!strcmp(method, "parse"))  return "tk_json_parse";
        if (!strcmp(method, "print"))  return "tk_json_print";
        if (!strcmp(method, "enc"))    return "json_enc";
        if (!strcmp(method, "dec"))    return "json_dec";
    }
    /* std.toon functions */
    if (!strcmp(mod, "toon")) {
        if (!strcmp(method, "enc"))       return "toon_enc";
        if (!strcmp(method, "dec"))       return "toon_dec";
        if (!strcmp(method, "str"))       return "toon_str";
        if (!strcmp(method, "i64"))       return "toon_i64";
        if (!strcmp(method, "f64"))       return "toon_f64";
        if (!strcmp(method, "bool"))      return "toon_bool";
        if (!strcmp(method, "arr"))       return "toon_arr";
        if (!strcmp(method, "from_json")) return "toon_from_json";
        if (!strcmp(method, "to_json"))   return "toon_to_json";
    }
    /* std.yaml functions */
    if (!strcmp(mod, "yaml")) {
        if (!strcmp(method, "enc"))       return "yaml_enc";
        if (!strcmp(method, "dec"))       return "yaml_dec";
        if (!strcmp(method, "str"))       return "yaml_str";
        if (!strcmp(method, "i64"))       return "yaml_i64";
        if (!strcmp(method, "f64"))       return "yaml_f64";
        if (!strcmp(method, "bool"))      return "yaml_bool";
        if (!strcmp(method, "arr"))       return "yaml_arr";
        if (!strcmp(method, "from_json")) return "yaml_from_json";
        if (!strcmp(method, "to_json"))   return "yaml_to_json";
    }
    /* std.i18n functions */
    if (!strcmp(mod, "i18n")) {
        if (!strcmp(method, "load"))   return "i18n_load";
        if (!strcmp(method, "get"))    return "i18n_get";
        if (!strcmp(method, "fmt"))    return "i18n_fmt";
        if (!strcmp(method, "locale")) return "i18n_locale";
    }
    /* std.str functions */
    if (!strcmp(mod, "str")) {
        if (!strcmp(method, "argv"))   return "tk_str_argv";
        if (!strcmp(method, "len"))    return "str_len";
        if (!strcmp(method, "concat")) return "str_concat";
        if (!strcmp(method, "split"))  return "str_split";
        if (!strcmp(method, "trim"))   return "str_trim";
        if (!strcmp(method, "upper"))  return "str_upper";
        if (!strcmp(method, "lower"))  return "str_lower";
    }
    return NULL;
}

/* Append to the string globals buffer (emitted at module scope before functions) */
static void str_buf_append(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rem = (int)sizeof(c->str_globals) - c->str_globals_len;
    if (rem > 0) {
        int n = vsnprintf(c->str_globals + c->str_globals_len, (size_t)rem, fmt, ap);
        if (n > 0) c->str_globals_len += (n < rem) ? n : rem - 1;
    }
    va_end(ap);
}

static int emit_str_global(Ctx *c, const char *raw, int rlen)
{
    const char *inner = raw + 1; int ilen = rlen-2; if(ilen<0)ilen=0;
    int idx = next_str(c);
    str_buf_append(c, "@.str.%d = private unnamed_addr constant [%d x i8] c\"",idx,ilen+1);
    for(int i=0;i<ilen;i++){
        unsigned char ch=(unsigned char)inner[i];
        if(ch=='\\'&&i+1<ilen){char nx=inner[i+1];
            if(nx=='n'){str_buf_append(c,"\\0A");i++;continue;}
            if(nx=='t'){str_buf_append(c,"\\09");i++;continue;}
            if(nx=='\\'){str_buf_append(c,"\\5C");i++;continue;}
            if(nx=='"'){str_buf_append(c,"\\22");i++;continue;}
        }
        if(ch>=32&&ch<127&&ch!='"'&&ch!='\\') {
            char tmp[2] = {(char)ch, 0};
            str_buf_append(c, "%s", tmp);
        } else {
            str_buf_append(c, "\\%02X", ch);
        }
    }
    str_buf_append(c, "\\00\"\n");
    return idx;
}

/* ── Expression emission ───────────────────────────────────────────── */

static int emit_expr(Ctx *c, const Node *n);
static void emit_stmt(Ctx *c, const Node *n);
static void set_local_type(Ctx *c, const char *name, const char *ty);
static const char *get_local_type(Ctx *c, const char *name);
static const char *expr_llvm_type(Ctx *c, const Node *n);
static const char *get_llvm_name(Ctx *c, const char *toke_name);
static const char *make_unique_name(Ctx *c, const char *toke_name);

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
        /* true/false are bool constants, not variable loads */
        if (!strcmp(tb, "true")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i1 0, 1\n", t);
            return t;
        }
        if (!strcmp(tb, "false")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = add i1 0, 0\n", t);
            return t;
        }
        t = next_tmp(c);
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            fprintf(c->out, "  %%t%d = load %s, ptr %%%s\n", t, lty, ln);
        }
        return t;
    case NODE_BINARY_EXPR: {
        int lhs=emit_expr(c,n->children[0]), rhs=emit_expr(c,n->children[1]);
        const char *lty = expr_llvm_type(c, n->children[0]);
        const char *rty = expr_llvm_type(c, n->children[1]);
        /* Array/string concat: ptr + ptr → call tk_array_concat */
        if (n->op == TK_PLUS && !strcmp(lty, "ptr") && !strcmp(rty, "ptr")) {
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call ptr @tk_array_concat(ptr %%t%d, ptr %%t%d)\n", t, lhs, rhs);
            return t;
        }
        /* ptr + i64 or i64 + ptr with + → also array concat (e.g. arr + [x]) */
        if (n->op == TK_PLUS && (!strcmp(lty, "ptr") || !strcmp(rty, "ptr"))) {
            /* Coerce the non-ptr side to ptr */
            if (!strcmp(lty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, lhs);
                lhs = z;
            }
            if (!strcmp(rty, "i64")) {
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, rhs);
                rhs = z;
            }
            t = next_tmp(c);
            fprintf(c->out, "  %%t%d = call ptr @tk_array_concat(ptr %%t%d, ptr %%t%d)\n", t, lhs, rhs);
            return t;
        }
        /* Coerce i1 operands to i64 for arithmetic/comparisons */
        if (!strcmp(lty, "i1")) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, lhs);
            lhs = z;
        } else if (!strcmp(lty, "ptr")) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, lhs);
            lhs = z;
        }
        if (!strcmp(rty, "i1")) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, rhs);
            rhs = z;
        } else if (!strcmp(rty, "ptr")) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, rhs);
            rhs = z;
        }
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
        /* Check for qualified call: children[0] is NODE_FIELD_EXPR for j.parse(...) */
        const char *resolved_fn = NULL;
        char fn_buf[256];
        if (n->children[0]->kind == NODE_FIELD_EXPR) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            resolved_fn = resolve_stdlib_call(c, alias, method);
            if (!resolved_fn) {
                /* Not a known stdlib call — build a mangled name */
                snprintf(fn_buf, sizeof fn_buf, "%s_%s", alias, method);
                resolved_fn = fn_buf;
            }
        }

        if (!resolved_fn) {
            tok_cp(c->src, n->children[0], tb, sizeof tb);
            if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
        } else {
            strncpy(tb, resolved_fn, sizeof tb - 1); tb[sizeof tb - 1] = '\0';
        }

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
        const char *arg_tys[NODE_MAX_CHILDREN];
        for (int i = 0; i < na; i++) {
            args[i] = emit_expr(c, n->children[i+1]);
            arg_tys[i] = expr_llvm_type(c, n->children[i+1]);
        }
        const FnSig *callee = lookup_fn(c, tb);

        /* For stdlib calls, determine return/param types by convention */
        const char *callee_ret = callee ? callee->ret : "i64";
        if (!callee && resolved_fn) {
            /* stdlib return types: tk_str_argv returns ptr, tk_json_parse returns i64, tk_json_print is void */
            if (!strcmp(tb, "tk_str_argv")) callee_ret = "ptr";
            else if (!strcmp(tb, "tk_json_print")) callee_ret = "void";
            else callee_ret = "i64";
        }

        /* Type-aware j.print dispatch: redirect to typed print function */
        if (!strcmp(tb, "tk_json_print") && na >= 1) {
            const char *aty0 = arg_tys[0];
            if (!strcmp(aty0, "i1")) {
                /* bool: zext to i64 and call tk_json_print_bool */
                int z = next_tmp(c);
                fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, args[0]);
                fprintf(c->out, "  call void @tk_json_print_bool(i64 %%t%d)\n", z);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "ptr")) {
                /* ptr: call tk_json_print_arr */
                fprintf(c->out, "  call void @tk_json_print_arr(ptr %%t%d)\n", args[0]);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            } else if (!strcmp(aty0, "double")) {
                fprintf(c->out, "  call void @tk_json_print_f64(double %%t%d)\n", args[0]);
                t = next_tmp(c);
                fprintf(c->out, "  %%t%d = add i64 0, 0 ; void call result\n", t);
                return t;
            }
            /* else: fall through to default i64 print */
        }

        /* Coerce each argument to its expected type if needed */
        for (int i = 0; i < na; i++) {
            const char *aty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
            if (!callee && resolved_fn) aty = "i64";
            if (!callee && resolved_fn && !strcmp(tb, "tk_json_parse")) aty = "ptr";
            if (!callee && resolved_fn && !strcmp(tb, "tk_str_argv")) aty = "i64";
            if (strcmp(aty, arg_tys[i])) {
                int z = next_tmp(c);
                if (!strcmp(arg_tys[i], "i1") && !strcmp(aty, "i64")) {
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, args[i]);
                } else if (!strcmp(arg_tys[i], "ptr") && !strcmp(aty, "i64")) {
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", z, args[i]);
                } else if (!strcmp(arg_tys[i], "i64") && !strcmp(aty, "ptr")) {
                    fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", z, args[i]);
                } else if (!strcmp(arg_tys[i], "i64") && !strcmp(aty, "i1")) {
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, args[i]);
                } else if (!strcmp(arg_tys[i], "i64") && !strcmp(aty, "double")) {
                    fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", z, args[i]);
                } else if (!strcmp(arg_tys[i], "double") && !strcmp(aty, "i64")) {
                    fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", z, args[i]);
                } else {
                    fprintf(c->out, "  %%t%d = add i64 0, %%t%d ; type coerce fallback\n", z, args[i]);
                }
                args[i] = z;
            }
        }

        if (!strcmp(callee_ret, "void")) {
            fprintf(c->out, "  call void @%s(", tb);
            for (int i = 0; i < na; i++) {
                if (i) fputc(',', c->out);
                const char *aty = (callee && i < callee->param_count) ? callee->param_tys[i] : "i64";
                if (!callee && resolved_fn) aty = "i64";
                if (!callee && resolved_fn && !strcmp(tb, "tk_json_parse")) aty = "ptr";
                if (!callee && resolved_fn && !strcmp(tb, "tk_str_argv")) aty = "i64";
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
            if (!callee && resolved_fn) aty = "i64";
            if (!callee && resolved_fn && !strcmp(tb, "tk_json_parse")) aty = "ptr";
            if (!callee && resolved_fn && !strcmp(tb, "tk_str_argv")) aty = "i64";
            fprintf(c->out, " %s %%t%d", aty, args[i]);
        }
        fputs(")\n", c->out);
        return t;
    }
    case NODE_CAST_EXPR: {
        int v = emit_expr(c, n->children[0]);
        const char *src_ty = expr_llvm_type(c, n->children[0]);
        t = next_tmp(c);
        if (n->child_count >= 2 && n->children[1]) {
            char tn[64]; tok_cp(c->src, n->children[1], tn, sizeof tn);
            if (!strcmp(tn, "f64")) {
                if (!strcmp(src_ty, "double")) {
                    fprintf(c->out, "  %%t%d = fadd double 0.0, %%t%d\n", t, v);
                } else {
                    fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", t, v);
                }
                return t;
            } else if (!strcmp(tn, "i64") || !strcmp(tn, "u64")) {
                if (!strcmp(src_ty, "double")) {
                    fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", t, v);
                } else if (!strcmp(src_ty, "i1")) {
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", t, v);
                } else if (!strcmp(src_ty, "ptr")) {
                    fprintf(c->out, "  %%t%d = ptrtoint ptr %%t%d to i64\n", t, v);
                } else {
                    /* already i64 — pass through */
                    fprintf(c->out, "  %%t%d = add i64 0, %%t%d\n", t, v);
                }
                return t;
            } else if (!strcmp(tn, "bool")) {
                if (!strcmp(src_ty, "i1")) {
                    fprintf(c->out, "  %%t%d = add i1 0, %%t%d\n", t, v);
                } else {
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", t, v);
                }
                return t;
            }
        }
        /* Array or unknown cast — treat as ptr identity (inttoptr or passthrough) */
        if (!strcmp(src_ty, "ptr")) {
            fprintf(c->out, "  %%t%d = getelementptr i8, ptr %%t%d, i32 0 ; as-cast ptr\n", t, v);
        } else {
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr ; as-cast to ptr\n", t, v);
        }
        return t;
    }
    case NODE_FIELD_EXPR: {
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        int base = emit_expr(c, n->children[0]);

        /* .len on arrays: length is stored at ptr[-1] */
        if (!strcmp(fn, "len")) {
            const char *bty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(bty, "i64")) {
                int conv = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", conv, base);
                base = conv;
            }
            t2 = next_tmp(c); t = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i32 -1 ; .len\n", t2, base);
            fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
            return t;
        }

        /* Struct field access */
        int fidx = 0;
        const StructInfo *si = resolve_base_struct(c, n->children[0]);
        if (si) fidx = struct_field_index(si, fn);
        {
            const char *bty = expr_llvm_type(c, n->children[0]);
            if (!strcmp(bty, "i64")) {
                int conv = next_tmp(c);
                fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", conv, base);
                base = conv;
            }
        }
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i32 %d ; .%s\n", t2, base, fidx, fn);
        fprintf(c->out, "  %%t%d = load i64, ptr %%t%d\n", t, t2);
        return t;
    }
    case NODE_INDEX_EXPR: {
        int base = emit_expr(c, n->children[0]);
        const char *bty = expr_llvm_type(c, n->children[0]);
        /* If base is i64 (e.g. from j.parse), inttoptr to ptr first */
        if (!strcmp(bty, "i64")) {
            int conv = next_tmp(c);
            fprintf(c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", conv, base);
            base = conv;
        }
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
        /* Allocate len+1 slots: [length | data[0] | data[1] | ...].
         * Return pointer to data[0] so that ptr[-1] == length. */
        int block = next_tmp(c);
        fprintf(c->out, "  %%t%d = alloca i64, i64 %d ; array block (len + %d elems)\n",
                block, n->child_count + 1, n->child_count);
        /* Store length at index 0 of the block */
        t2 = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i64 0\n", t2, block);
        fprintf(c->out, "  store i64 %d, ptr %%t%d ; .len\n", n->child_count, t2);
        /* Data pointer = block + 1 */
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i64 1 ; data start\n", t, block);
        for (int i = 0; i < n->child_count; i++) {
            int ev = emit_expr(c, n->children[i]);
            t3 = next_tmp(c);
            fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i64 %d\n", t3, t, i);
            fprintf(c->out, "  store i64 %%t%d, ptr %%t%d\n", ev, t3);
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

/* Track the LLVM type of a local variable (for correct load/store). */
/* Get the current LLVM name for a toke variable (latest alias) */
static const char *get_llvm_name(Ctx *c, const char *toke_name) {
    /* Search aliases in reverse order to find the latest */
    for (int i = c->alias_count - 1; i >= 0; i--)
        if (!strcmp(c->aliases[i].toke_name, toke_name))
            return c->aliases[i].llvm_name;
    return toke_name; /* no alias, use original */
}

/* Register a new unique LLVM name for a toke variable.
 * If the toke name has been used before, appends .N suffix. */
static const char *make_unique_name(Ctx *c, const char *toke_name) {
    /* Check if this name already exists in locals */
    int exists = 0;
    for (int i = 0; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, toke_name) ||
            (strlen(c->locals[i].name) > strlen(toke_name) &&
             !strncmp(c->locals[i].name, toke_name, strlen(toke_name)) &&
             c->locals[i].name[strlen(toke_name)] == '.'))
            exists = 1;
    if (!exists) return toke_name; /* first use, no renaming needed */

    /* Generate a unique name */
    if (c->alias_count >= MAX_LOCALS) return toke_name;
    NameAlias *a = &c->aliases[c->alias_count++];
    strncpy(a->toke_name, toke_name, 127); a->toke_name[127] = '\0';
    snprintf(a->llvm_name, 128, "%s.%d", toke_name, ++c->name_scope);
    return a->llvm_name;
}

static void set_local_type(Ctx *c, const char *name, const char *ty) {
    /* Update existing entry if present */
    for (int i = 0; i < c->local_count; i++) {
        if (!strcmp(c->locals[i].name, name)) { c->locals[i].ty = ty; return; }
    }
    if (c->local_count >= MAX_LOCALS) return;
    LocalType *lt = &c->locals[c->local_count++];
    strncpy(lt->name, name, 127); lt->name[127] = '\0';
    lt->ty = ty;
}
static const char *get_local_type(Ctx *c, const char *name) {
    for (int i = 0; i < c->local_count; i++)
        if (!strcmp(c->locals[i].name, name)) return c->locals[i].ty;
    if (is_ptr_local(c, name)) return "ptr";
    return "i64";
}

/* Return the LLVM IR type string that emit_expr will produce for node n.
 * This is used at type boundaries (let, assign, return, call args) to emit
 * correct store/ret instructions. */
static const char *expr_llvm_type(Ctx *c, const Node *n) {
    if (!n) return "i64";
    switch (n->kind) {
    case NODE_BOOL_LIT:
        return "i1";
    case NODE_FLOAT_LIT:
        return "double";
    case NODE_STR_LIT:
        return "ptr";
    case NODE_INT_LIT:
        return "i64";
    case NODE_STRUCT_LIT:
    case NODE_ARRAY_LIT:
    case NODE_MAP_LIT:
        return "ptr";
    case NODE_IDENT: {
        char nb[128]; tok_cp(c->src, n, nb, sizeof nb);
        if (!strcmp(nb, "true") || !strcmp(nb, "false")) return "i1";
        const char *ln = get_llvm_name(c, nb);
        return get_local_type(c, ln);
    }
    case NODE_BINARY_EXPR:
        switch (n->op) {
        case TK_LT: case TK_GT: case TK_EQ: return "i1";
        case TK_PLUS: {
            const char *lt = expr_llvm_type(c, n->children[0]);
            const char *rt = expr_llvm_type(c, n->children[1]);
            if (!strcmp(lt, "ptr") || !strcmp(rt, "ptr")) return "ptr";
            return "i64";
        }
        default: return "i64";
        }
    case NODE_UNARY_EXPR:
        if (n->op == TK_BANG) return "i1";
        return "i64";
    case NODE_CALL_EXPR: {
        if (n->child_count < 1) return "i64";
        /* Check for resolved stdlib calls */
        if (n->children[0]->kind == NODE_FIELD_EXPR) {
            char alias[128], method[128];
            tok_cp(c->src, n->children[0]->children[0], alias, sizeof alias);
            tok_cp(c->src, n->children[0]->children[1], method, sizeof method);
            const char *resolved = resolve_stdlib_call(c, alias, method);
            if (resolved) {
                if (!strcmp(resolved, "tk_str_argv")) return "ptr";
                if (!strcmp(resolved, "tk_json_print")) return "i64"; /* void, but wrapped */
                return "i64"; /* tk_json_parse returns i64 */
            }
        }
        char fn[128]; tok_cp(c->src, n->children[0], fn, sizeof fn);
        const FnSig *sig = lookup_fn(c, fn);
        if (sig) return sig->ret;
        return "i64";
    }
    case NODE_CAST_EXPR: {
        if (n->child_count >= 2 && n->children[1]) {
            char tn[64]; tok_cp(c->src, n->children[1], tn, sizeof tn);
            if (!strcmp(tn, "f64")) return "double";
            if (!strcmp(tn, "i64") || !strcmp(tn, "u64")) return "i64";
            if (!strcmp(tn, "bool")) return "i1";
        }
        return "ptr"; /* array / unknown cast → inttoptr */
    }
    case NODE_INDEX_EXPR:
        return "i64"; /* array element load */
    case NODE_FIELD_EXPR: {
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        if (!strcmp(fn, "len")) return "i64";
        return "i64";
    }
    default:
        return "i64";
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
        { const char *uname = make_unique_name(c, tb);
          if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
        }
        if (n->child_count >= 2) {
            const char *vty = expr_llvm_type(c, n->children[1]);
            if (!strcmp(vty, "ptr")) {
                const char *stype = expr_struct_type(c, n->children[1]);
                mark_ptr_with_type(c, tb, stype);
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", vty, v, tb);
        } else {
            set_local_type(c, tb, "i64");
            fprintf(c->out, "  %%%s = alloca i64\n", tb);
        }
        break;
    case NODE_ASSIGN_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        {
            const char *ln = get_llvm_name(c, tb);
            const char *lty = get_local_type(c, ln);
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", lty, v, ln);
        }
        break;
    case NODE_RETURN_STMT:
        if (n->child_count > 0) {
            const char *rt = c->cur_fn_ret ? c->cur_fn_ret : "i64";
            if (!strcmp(rt, "void")) {
                /* void function with <expr — emit the expr for side effects, return void */
                emit_expr(c, n->children[0]);
                fputs("  ret void\n", c->out);
            } else {
                int v = emit_expr(c, n->children[0]);
                const char *ety = expr_llvm_type(c, n->children[0]);
                /* Coerce if needed: e.g. i1→i64, i64→i1 */
                if (!strcmp(ety, rt)) {
                    fprintf(c->out, "  ret %s %%t%d\n", rt, v);
                } else if (!strcmp(ety, "i1") && !strcmp(rt, "i64")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = zext i1 %%t%d to i64\n", z, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i64") && !strcmp(rt, "i1")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = trunc i64 %%t%d to i1\n", z, v);
                    fprintf(c->out, "  ret i1 %%t%d\n", z);
                } else if (!strcmp(ety, "double") && !strcmp(rt, "i64")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = fptosi double %%t%d to i64\n", z, v);
                    fprintf(c->out, "  ret i64 %%t%d\n", z);
                } else if (!strcmp(ety, "i64") && !strcmp(rt, "double")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = sitofp i64 %%t%d to double\n", z, v);
                    fprintf(c->out, "  ret double %%t%d\n", z);
                } else {
                    fprintf(c->out, "  ret %s %%t%d\n", rt, v);
                }
            }
        } else {
            fputs("  ret void\n", c->out);
        }
        c->term = 1;
        break;
    case NODE_BREAK_STMT:
        fprintf(c->out, "  br label %%loop_exit%d\n", c->break_lbl);
        c->term = 1;
        break;
    case NODE_IF_STMT: {
        int L = next_lbl(c);
        int cv = emit_expr(c, n->children[0]);
        { const char *ct = expr_llvm_type(c, n->children[0]);
          if (strcmp(ct, "i1")) {
            int z = next_tmp(c);
            fprintf(c->out, "  %%t%d = icmp ne %s %%t%d, 0\n", z, ct, cv);
            cv = z;
          }
        }
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
    case NODE_LOOP_INIT: {
        /* lp(let i=0; ...) — alloca + store for the loop variable */
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        { const char *uname = make_unique_name(c, tb);
          if (uname != tb) strncpy(tb, uname, sizeof tb - 1);
        }
        if (n->child_count >= 2) {
            const char *vty = expr_llvm_type(c, n->children[1]);
            if (!strcmp(vty, "ptr")) {
                const char *stype = expr_struct_type(c, n->children[1]);
                mark_ptr_with_type(c, tb, stype);
            }
            set_local_type(c, tb, vty);
            fprintf(c->out, "  %%%s = alloca %s\n", tb, vty);
            int v = emit_expr(c, n->children[1]);
            fprintf(c->out, "  store %s %%t%d, ptr %%%s\n", vty, v, tb);
        } else {
            set_local_type(c, tb, "i64");
            fprintf(c->out, "  %%%s = alloca i64\n", tb);
        }
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
                { const char *ct = expr_llvm_type(c, maybe_cond);
                  if (strcmp(ct, "i1")) {
                    int z = next_tmp(c);
                    fprintf(c->out, "  %%t%d = icmp ne %s %%t%d, 0\n", z, ct, cv);
                    cv = z;
                  }
                }
                fprintf(c->out, "  br i1 %%t%d, label %%loop_body%d, label %%loop_exit%d\n", cv, L, L);
            } else {
                fprintf(c->out, "  br label %%loop_body%d\n", L);
            }
        } else {
            fprintf(c->out, "  br label %%loop_body%d\n", L);
        }
        fprintf(c->out, "loop_body%d:\n", L);
        int save_break = c->break_lbl; c->break_lbl = L;
        c->term = 0;
        emit_stmt(c, body);
        c->break_lbl = save_break;
        /* emit loop step (e.g. i=i+1) before branching back to header */
        if (n->child_count >= 3) {
            const Node *maybe_step = n->children[n->child_count - 2];
            if (maybe_step->kind == NODE_ASSIGN_STMT)
                emit_stmt(c, maybe_step);
        }
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
        /* Rename toke main to tk_main to avoid collision with C main wrapper */
        if (!strcmp(tb, "main")) strcpy(tb, "tk_main");
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
        c->local_count = 0; /* reset local type tracking */
        c->alias_count = 0; c->name_scope = 0; /* reset variable scoping */
        c->cur_fn_ret = ret;
        fprintf(c->out, "\ndefine %s @%s(", ret, tb);
        int first = 1;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            if (!first) fputs(", ", c->out);
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
            fprintf(c->out, "%s %%%s.arg", pty, pn);
            first = 0;
        }
        fputs(") {\nentry:\n", c->out);
        /* Spill params */
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            const char *pty = "i64";
            if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                pty = resolve_llvm_type(c, n->children[i]->children[1]);
            if (!strcmp(pty, "ptr")) {
                char pty_name[128] = "";
                if (n->children[i]->child_count > 1 && n->children[i]->children[1])
                    tok_cp(c->src, n->children[i]->children[1], pty_name, sizeof pty_name);
                const char *stype = lookup_struct(c, pty_name) ? pty_name : NULL;
                mark_ptr_with_type(c, pn, stype);
            }
            set_local_type(c, pn, pty);
            fprintf(c->out, "  %%%s = alloca %s\n  store %s %%%s.arg, ptr %%%s\n", pn, pty, pty, pn, pn);
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
    fputs("declare ptr @tk_spawn(ptr)\ndeclare i64 @tk_await(ptr)\n", f);
    /* Runtime declarations for std.json and std.str */
    fputs("declare i64 @tk_json_parse(ptr)\n", f);
    fputs("declare void @tk_json_print(i64)\n", f);
    fputs("declare ptr @tk_str_argv(i64)\n", f);
    fputs("declare void @tk_runtime_init(i32, ptr)\n", f);
    fputs("declare ptr @tk_array_concat(ptr, ptr)\n", f);
    fputs("declare ptr @tk_str_concat(ptr, ptr)\n", f);
    fputs("declare i64 @tk_str_len(ptr)\n", f);
    fputs("declare i64 @tk_str_char_at(ptr, i64)\n", f);
    fputs("declare void @tk_json_print_bool(i64)\n", f);
    fputs("declare void @tk_json_print_arr(ptr)\n", f);
    fputs("declare void @tk_json_print_str(ptr)\n\n", f);

    prepass_structs(&ctx, ast);
    prepass_funcs(&ctx, ast);
    prepass_imports(&ctx, ast);
    emit_toplevel(&ctx, ast);

    /* Flush buffered string globals (must appear at module scope, before main) */
    if (ctx.str_globals_len > 0)
        fwrite(ctx.str_globals, 1, (size_t)ctx.str_globals_len, f);

    /* Emit C-compatible main wrapper that inits runtime and calls tk_main */
    fputs("\ndefine i32 @main(i32 %argc, ptr %argv) {\n", f);
    fputs("  call void @tk_runtime_init(i32 %argc, ptr %argv)\n", f);
    /* Check if toke main returns void or i64 */
    const FnSig *tkmain = lookup_fn(&ctx, "tk_main");
    if (tkmain && !strcmp(tkmain->ret, "void")) {
        fputs("  call void @tk_main()\n", f);
        fputs("  ret i32 0\n", f);
    } else {
        fputs("  %r = call i64 @tk_main()\n", f);
        fputs("  %rc = trunc i64 %r to i32\n", f);
        fputs("  ret i32 %rc\n", f);
    }
    fputs("}\n", f);

    if (fflush(f) || ferror(f)) {
        fclose(f);
        diag_emit(DIAG_ERROR, E9002, 0, 0, 0,
                  "LLVM IR emission failed: I/O error writing .ll file", (void*)0);
        return -1;
    }
    fclose(f);
    return 0;
}

/* Find the tk_runtime.c source file.  Check relative to the tkc binary
 * (../src/stdlib/ or same-dir src/stdlib/) and TKC_RUNTIME_DIR env var. */
static const char *find_runtime_source(void) {
    static char path[512];
    const char *env = getenv("TKC_RUNTIME_DIR");
    if (env) {
        snprintf(path, sizeof path, "%s/tk_runtime.c", env);
        FILE *f = fopen(path, "r"); if (f) { fclose(f); return path; }
    }
    /* Try paths relative to known install locations */
    const char *candidates[] = {
        TKC_STDLIB_DIR "/tk_runtime.c",  /* set at compile time */
        "src/stdlib/tk_runtime.c",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "r");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

int compile_binary(const char *out_ll, const char *out_bin, const char *target)
{
    char cmd[2048];
    const char *rt = find_runtime_source();
    if (rt) {
        if(target&&target[0])
            snprintf(cmd,sizeof cmd,"clang -O1 -target %s -o %s %s %s",target,out_bin,out_ll,rt);
        else
            snprintf(cmd,sizeof cmd,"clang -O1 -o %s %s %s",out_bin,out_ll,rt);
    } else {
        if(target&&target[0])
            snprintf(cmd,sizeof cmd,"clang -O1 -target %s -o %s %s",target,out_bin,out_ll);
        else
            snprintf(cmd,sizeof cmd,"clang -O1 -o %s %s",out_bin,out_ll);
    }
    int rc = system(cmd);
    if(rc!=0){char msg[256];snprintf(msg,sizeof msg,"clang invocation failed with exit code %d",rc);
        diag_emit(DIAG_ERROR,E9003,0,0,0,msg,(void*)0);return -1;}
    return 0;
}
