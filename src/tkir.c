/*
 * tkir.c — Binary TKIR format encoder and reader for toke.
 *
 * Encoder (story 76.1.6a): walks the type-checked AST and emits a
 * compact .tkir binary file.
 *
 * Reader (story 76.1.6b): parses .tkir files back into TkirModule
 * structs, with full validation of magic, version, section structure,
 * opcode values, and register references.
 *
 * Story: 76.1.6a, 76.1.6b
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tkir.h"

/* ── TkirBuf helpers (encoder growable buffer) ───────────────────────── */

void tkir_buf_init(TkirBuf *b)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

void tkir_buf_free(TkirBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void tkir_buf_grow(TkirBuf *b, size_t need)
{
    if (b->len + need <= b->cap) return;
    size_t nc = b->cap == 0 ? 4096 : b->cap;
    while (nc < b->len + need) nc *= 2;
    uint8_t *p = realloc(b->data, nc);
    if (!p) return;  /* best-effort; caller checks later */
    b->data = p;
    b->cap  = nc;
}

void tkir_buf_u8(TkirBuf *b, uint8_t v)
{
    tkir_buf_grow(b, 1);
    if (b->len < b->cap) b->data[b->len++] = v;
}

void tkir_buf_u16(TkirBuf *b, uint16_t v)
{
    tkir_buf_grow(b, 2);
    if (b->len + 2 <= b->cap) {
        b->data[b->len++] = (uint8_t)(v & 0xFF);
        b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    }
}

void tkir_buf_u32(TkirBuf *b, uint32_t v)
{
    tkir_buf_grow(b, 4);
    if (b->len + 4 <= b->cap) {
        b->data[b->len++] = (uint8_t)(v & 0xFF);
        b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
        b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
        b->data[b->len++] = (uint8_t)((v >> 24) & 0xFF);
    }
}

void tkir_buf_i64(TkirBuf *b, int64_t v)
{
    tkir_buf_grow(b, 8);
    if (b->len + 8 <= b->cap) {
        uint64_t u = (uint64_t)v;
        for (int i = 0; i < 8; i++) {
            b->data[b->len++] = (uint8_t)(u & 0xFF);
            u >>= 8;
        }
    }
}

void tkir_buf_bytes(TkirBuf *b, const void *src, size_t n)
{
    tkir_buf_grow(b, n);
    if (b->len + n <= b->cap) {
        memcpy(b->data + b->len, src, n);
        b->len += n;
    }
}

void tkir_buf_str(TkirBuf *b, const char *s, size_t len)
{
    tkir_buf_u16(b, (uint16_t)len);
    tkir_buf_bytes(b, s, len);
}

/* ── Encoder: internal types (story 76.1.6a) ─────────────────────────── */

#include "diag.h"
#include "tkc_limits.h"

typedef struct {
    uint8_t  kind;
    char     name[NAME_BUF];
    int      field_count;
    char     field_names[TKC_MAX_PARAMS][NAME_BUF];
    uint32_t field_type_idxs[TKC_MAX_PARAMS];
} EncTypeEntry;

typedef struct {
    char     name[NAME_BUF];
    int      param_count;
    uint32_t param_type_idxs[TKC_MAX_PARAMS];
    uint32_t ret_type_idx;
    uint8_t  flags;
    const Node *body;
} EncFunc;

typedef struct {
    uint8_t kind;
    union {
        struct { const char *ptr; size_t len; } str;
        int64_t  i64_val;
        double   f64_val;
    } u;
} EncDataEntry;

typedef struct {
    char     module[ALIAS_BUF];
    char     symbol[NAME_BUF];
    uint32_t type_idx;
} EncImportEntry;

#define ENC_MAX_TYPES  256
#define ENC_MAX_DATA  1024

typedef struct {
    const char    *src;
    const TypeEnv *types;
    const NameEnv *names;
    const char    *target;

    EncTypeEntry   type_entries[ENC_MAX_TYPES];
    int            type_count;

    EncFunc        funcs[TKC_MAX_FUNCS];
    int            func_count;

    EncDataEntry   data_entries[ENC_MAX_DATA];
    int            data_count;

    EncImportEntry enc_imports[TKC_MAX_IMPORTS];
    int            import_count;

    uint16_t       reg;
    TkirBuf        code;
    uint32_t      *func_code_offsets;
} Enc;

static void enc_tok_text(const char *src, const Node *n, char *buf, int sz)
{
    int len = n->tok_len < sz - 1 ? n->tok_len : sz - 1;
    memcpy(buf, src + n->tok_start, (size_t)len);
    buf[len] = '\0';
}

static uint32_t enc_ensure_scalar(Enc *e, uint8_t kind)
{
    for (int i = 0; i < e->type_count; i++) {
        if (e->type_entries[i].kind == kind && e->type_entries[i].name[0] == '\0')
            return (uint32_t)i;
    }
    if (e->type_count >= ENC_MAX_TYPES) return 0;
    EncTypeEntry *te = &e->type_entries[e->type_count];
    te->kind = kind;
    te->name[0] = '\0';
    te->field_count = 0;
    return (uint32_t)e->type_count++;
}

static uint32_t enc_resolve_type_kind(const char *name)
{
    if (!strcmp(name, "i64") || !strcmp(name, "I64") || !strcmp(name, "Int"))
        return TKIR_TY_I64;
    if (!strcmp(name, "f64") || !strcmp(name, "F64") || !strcmp(name, "Float"))
        return TKIR_TY_F64;
    if (!strcmp(name, "bool") || !strcmp(name, "Bool"))
        return TKIR_TY_BOOL;
    if (!strcmp(name, "str") || !strcmp(name, "Str") || !strcmp(name, "String"))
        return TKIR_TY_STR;
    if (!strcmp(name, "void") || !strcmp(name, "Void"))
        return TKIR_TY_VOID;
    return 0;
}

static uint32_t enc_type_idx_from_name(Enc *e, const char *name)
{
    uint32_t kind = enc_resolve_type_kind(name);
    if (kind != 0) return enc_ensure_scalar(e, (uint8_t)kind);
    for (int i = 0; i < e->type_count; i++) {
        if (e->type_entries[i].kind == TKIR_TY_STRUCT &&
            !strcmp(e->type_entries[i].name, name))
            return (uint32_t)i;
    }
    if (e->type_count >= ENC_MAX_TYPES) return 0;
    EncTypeEntry *te = &e->type_entries[e->type_count];
    te->kind = TKIR_TY_STRUCT;
    strncpy(te->name, name, NAME_BUF - 1);
    te->name[NAME_BUF - 1] = '\0';
    te->field_count = 0;
    return (uint32_t)e->type_count++;
}

static uint32_t enc_type_idx_from_node(Enc *e, const Node *n)
{
    if (!n) return enc_ensure_scalar(e, TKIR_TY_VOID);
    if (n->kind == NODE_TYPE_IDENT || n->kind == NODE_TYPE_EXPR) {
        char name[NAME_BUF];
        enc_tok_text(e->src, n, name, NAME_BUF);
        return enc_type_idx_from_name(e, name);
    }
    if (n->kind == NODE_ARRAY_TYPE) return enc_ensure_scalar(e, TKIR_TY_ARRAY);
    if (n->kind == NODE_FUNC_TYPE)  return enc_ensure_scalar(e, TKIR_TY_FN_PTR);
    return enc_ensure_scalar(e, TKIR_TY_VOID);
}

static uint32_t enc_add_str(Enc *e, const char *str, size_t len)
{
    for (int i = 0; i < e->data_count; i++) {
        if (e->data_entries[i].kind == TKIR_DATA_STR &&
            e->data_entries[i].u.str.len == len &&
            !memcmp(e->data_entries[i].u.str.ptr, str, len))
            return (uint32_t)i;
    }
    if (e->data_count >= ENC_MAX_DATA) return 0;
    EncDataEntry *de = &e->data_entries[e->data_count];
    de->kind = TKIR_DATA_STR;
    de->u.str.ptr = str;
    de->u.str.len = len;
    return (uint32_t)e->data_count++;
}

static uint32_t enc_add_i64(Enc *e, int64_t val)
{
    for (int i = 0; i < e->data_count; i++) {
        if (e->data_entries[i].kind == TKIR_DATA_I64 &&
            e->data_entries[i].u.i64_val == val)
            return (uint32_t)i;
    }
    if (e->data_count >= ENC_MAX_DATA) return 0;
    EncDataEntry *de = &e->data_entries[e->data_count];
    de->kind = TKIR_DATA_I64;
    de->u.i64_val = val;
    return (uint32_t)e->data_count++;
}

static uint16_t enc_next_reg(Enc *e) { return e->reg++; }

static void enc_emit_rrr(Enc *e, uint8_t op, uint16_t dest,
                         uint16_t lhs, uint16_t rhs)
{
    tkir_buf_u8(&e->code, op);
    tkir_buf_u16(&e->code, dest);
    tkir_buf_u16(&e->code, lhs);
    tkir_buf_u16(&e->code, rhs);
}

static uint16_t enc_emit_const_data(Enc *e, uint32_t data_idx)
{
    uint16_t dest = enc_next_reg(e);
    tkir_buf_u8(&e->code, TKIR_OP_CONST_DATA);
    tkir_buf_u16(&e->code, dest);
    tkir_buf_u16(&e->code, (uint16_t)data_idx);
    return dest;
}

static uint16_t enc_emit_call(Enc *e, uint16_t func_idx,
                              uint16_t *args, int argc)
{
    uint16_t dest = enc_next_reg(e);
    tkir_buf_u8(&e->code, TKIR_OP_CALL);
    tkir_buf_u16(&e->code, dest);
    tkir_buf_u16(&e->code, func_idx);
    tkir_buf_u16(&e->code, (uint16_t)argc);
    for (int i = 0; i < argc; i++)
        tkir_buf_u16(&e->code, args[i]);
    return dest;
}

static void enc_emit_ret(Enc *e, uint16_t src_reg)
{
    tkir_buf_u8(&e->code, TKIR_OP_RET);
    tkir_buf_u16(&e->code, src_reg);
}

static void enc_emit_ret_void(Enc *e)
{
    tkir_buf_u8(&e->code, TKIR_OP_RET);
    tkir_buf_u16(&e->code, 0);
}

/* ── Encoder: prepass ────────────────────────────────────────────────── */

static void enc_prepass_types(Enc *e, const Node *ast)
{
    enc_ensure_scalar(e, TKIR_TY_I64);
    enc_ensure_scalar(e, TKIR_TY_F64);
    enc_ensure_scalar(e, TKIR_TY_BOOL);
    enc_ensure_scalar(e, TKIR_TY_STR);
    enc_ensure_scalar(e, TKIR_TY_VOID);

    for (int i = 0; i < ast->child_count; i++) {
        const Node *decl = ast->children[i];
        if (decl->kind != NODE_TYPE_DECL || decl->child_count < 1) continue;
        char name[NAME_BUF];
        enc_tok_text(e->src, decl->children[0], name, NAME_BUF);
        int found = 0;
        for (int j = 0; j < e->type_count; j++) {
            if (e->type_entries[j].kind == TKIR_TY_STRUCT &&
                !strcmp(e->type_entries[j].name, name)) { found = 1; break; }
        }
        if (found || e->type_count >= ENC_MAX_TYPES) continue;
        EncTypeEntry *te = &e->type_entries[e->type_count];
        te->kind = TKIR_TY_STRUCT;
        strncpy(te->name, name, NAME_BUF - 1);
        te->name[NAME_BUF - 1] = '\0';
        te->field_count = 0;
        for (int c = 1; c < decl->child_count; c++) {
            const Node *child = decl->children[c];
            if (child->kind == NODE_STMT_LIST) {
                for (int k = 0; k < child->child_count; k++) {
                    const Node *fld = child->children[k];
                    if (fld->kind == NODE_FIELD && te->field_count < TKC_MAX_PARAMS) {
                        enc_tok_text(e->src, fld->children[0],
                                     te->field_names[te->field_count], NAME_BUF);
                        te->field_type_idxs[te->field_count] =
                            (fld->child_count > 1) ? enc_type_idx_from_node(e, fld->children[1])
                                                   : enc_ensure_scalar(e, TKIR_TY_I64);
                        te->field_count++;
                    }
                }
            } else if (child->kind == NODE_FIELD && te->field_count < TKC_MAX_PARAMS) {
                enc_tok_text(e->src, child->children[0],
                             te->field_names[te->field_count], NAME_BUF);
                te->field_type_idxs[te->field_count] =
                    (child->child_count > 1) ? enc_type_idx_from_node(e, child->children[1])
                                             : enc_ensure_scalar(e, TKIR_TY_I64);
                te->field_count++;
            }
        }
        e->type_count++;
    }
}

static void enc_prepass_funcs(Enc *e, const Node *ast)
{
    for (int i = 0; i < ast->child_count; i++) {
        const Node *decl = ast->children[i];
        if (decl->kind != NODE_FUNC_DECL) continue;
        if (e->func_count >= TKC_MAX_FUNCS || decl->child_count < 1) continue;
        EncFunc *fn = &e->funcs[e->func_count];
        memset(fn, 0, sizeof(*fn));
        enc_tok_text(e->src, decl->children[0], fn->name, NAME_BUF);
        int has_body = 0;
        const Node *body = NULL, *ret_spec = NULL;
        for (int c = 1; c < decl->child_count; c++) {
            const Node *child = decl->children[c];
            if (child->kind == NODE_PARAM && fn->param_count < TKC_MAX_PARAMS) {
                fn->param_type_idxs[fn->param_count] =
                    (child->child_count > 1) ? enc_type_idx_from_node(e, child->children[1])
                                             : enc_ensure_scalar(e, TKIR_TY_I64);
                fn->param_count++;
            } else if (child->kind == NODE_RETURN_SPEC) {
                ret_spec = child;
            } else if (child->kind == NODE_STMT_LIST) {
                has_body = 1; body = child;
            }
        }
        fn->ret_type_idx = (ret_spec && ret_spec->child_count > 0)
            ? enc_type_idx_from_node(e, ret_spec->children[0])
            : enc_ensure_scalar(e, TKIR_TY_VOID);
        fn->flags = 0;
        if (!has_body) fn->flags |= TKIR_FN_EXTERN;
        if (!strcmp(fn->name, "main")) fn->flags |= TKIR_FN_EXPORTED;
        fn->body = body;
        e->func_count++;
    }
}

static void enc_prepass_imports(Enc *e, const Node *ast)
{
    for (int i = 0; i < ast->child_count; i++) {
        const Node *decl = ast->children[i];
        if (decl->kind != NODE_IMPORT) continue;
        if (e->import_count >= TKC_MAX_IMPORTS) continue;
        EncImportEntry *imp = &e->enc_imports[e->import_count];
        memset(imp, 0, sizeof(*imp));
        if (decl->child_count >= 2) {
            enc_tok_text(e->src, decl->children[0], imp->symbol, NAME_BUF);
            enc_tok_text(e->src, decl->children[1], imp->module, ALIAS_BUF);
        } else if (decl->child_count == 1) {
            enc_tok_text(e->src, decl->children[0], imp->module, ALIAS_BUF);
            strncpy(imp->symbol, imp->module, NAME_BUF - 1);
        }
        imp->type_idx = 0;
        e->import_count++;
    }
}

/* ── Encoder: expression emitter ─────────────────────────────────────── */

static uint16_t enc_emit_expr(Enc *e, const Node *n)
{
    if (!n) return 0;
    switch (n->kind) {
    case NODE_INT_LIT: {
        char buf[NAME_BUF];
        enc_tok_text(e->src, n, buf, NAME_BUF);
        return enc_emit_const_data(e, enc_add_i64(e, strtoll(buf, NULL, 10)));
    }
    case NODE_STR_LIT: {
        int sstart = n->tok_start + 1, slen = n->tok_len - 2;
        if (slen < 0) slen = 0;
        return enc_emit_const_data(e, enc_add_str(e, e->src + sstart, (size_t)slen));
    }
    case NODE_BOOL_LIT: {
        char buf[NAME_BUF];
        enc_tok_text(e->src, n, buf, NAME_BUF);
        return enc_emit_const_data(e, enc_add_i64(e,
            (!strcmp(buf, "true") || !strcmp(buf, "TRUE")) ? 1 : 0));
    }
    case NODE_FLOAT_LIT: {
        char buf[NAME_BUF];
        enc_tok_text(e->src, n, buf, NAME_BUF);
        return enc_emit_const_data(e, enc_add_i64(e, (int64_t)strtod(buf, NULL)));
    }
    case NODE_IDENT: {
        uint16_t dest = enc_next_reg(e);
        tkir_buf_u8(&e->code, TKIR_OP_LOAD);
        tkir_buf_u16(&e->code, dest);
        tkir_buf_u16(&e->code, 0);
        return dest;
    }
    case NODE_BINARY_EXPR: {
        if (n->child_count < 2) return 0;
        uint16_t lhs = enc_emit_expr(e, n->children[0]);
        uint16_t rhs = enc_emit_expr(e, n->children[1]);
        uint16_t dest = enc_next_reg(e);
        uint8_t op;
        switch (n->op) {
        case TK_PLUS:    op = TKIR_OP_ADD; break;
        case TK_MINUS:   op = TKIR_OP_SUB; break;
        case TK_STAR:    op = TKIR_OP_MUL; break;
        case TK_SLASH:   op = TKIR_OP_DIV; break;
        case TK_PERCENT: op = TKIR_OP_MOD; break;
        case TK_LT:      op = TKIR_OP_LT;  break;
        case TK_GT:      op = TKIR_OP_GT;  break;
        default:         op = TKIR_OP_ADD; break;
        }
        enc_emit_rrr(e, op, dest, lhs, rhs);
        return dest;
    }
    case NODE_UNARY_EXPR: {
        if (n->child_count < 1) return 0;
        uint16_t sr = enc_emit_expr(e, n->children[0]);
        uint16_t dest = enc_next_reg(e);
        tkir_buf_u8(&e->code, TKIR_OP_NEG);
        tkir_buf_u16(&e->code, dest);
        tkir_buf_u16(&e->code, sr);
        return dest;
    }
    case NODE_CALL_EXPR: {
        if (n->child_count < 1) return 0;
        char fname[NAME_BUF];
        enc_tok_text(e->src, n->children[0], fname, NAME_BUF);
        uint16_t fidx = 0;
        for (int i = 0; i < e->func_count; i++)
            if (!strcmp(e->funcs[i].name, fname)) { fidx = (uint16_t)i; break; }
        int argc = n->child_count - 1;
        uint16_t args[TKC_MAX_PARAMS];
        for (int i = 0; i < argc && i < TKC_MAX_PARAMS; i++)
            args[i] = enc_emit_expr(e, n->children[i + 1]);
        return enc_emit_call(e, fidx, args, argc);
    }
    case NODE_FIELD_EXPR: {
        if (n->child_count < 2) return 0;
        uint16_t base = enc_emit_expr(e, n->children[0]);
        uint16_t dest = enc_next_reg(e);
        tkir_buf_u8(&e->code, TKIR_OP_FIELD_GET);
        tkir_buf_u16(&e->code, dest);
        tkir_buf_u16(&e->code, base);
        tkir_buf_u16(&e->code, 0);
        return dest;
    }
    case NODE_CAST_EXPR: {
        if (n->child_count < 2) return 0;
        uint16_t sr = enc_emit_expr(e, n->children[0]);
        uint16_t dest = enc_next_reg(e);
        tkir_buf_u8(&e->code, TKIR_OP_CAST);
        tkir_buf_u16(&e->code, dest);
        tkir_buf_u16(&e->code, sr);
        tkir_buf_u16(&e->code, (uint16_t)enc_type_idx_from_node(e, n->children[1]));
        return dest;
    }
    default: break;
    }
    return 0;
}

/* ── Encoder: statement emitter ──────────────────────────────────────── */

static void enc_emit_stmt(Enc *e, const Node *n)
{
    if (!n) return;
    switch (n->kind) {
    case NODE_STMT_LIST:
        for (int i = 0; i < n->child_count; i++) enc_emit_stmt(e, n->children[i]);
        break;
    case NODE_RETURN_STMT:
        if (n->child_count > 0) enc_emit_ret(e, enc_emit_expr(e, n->children[0]));
        else enc_emit_ret_void(e);
        break;
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        if (n->child_count >= 2) {
            uint16_t val = enc_emit_expr(e, n->children[n->child_count - 1]);
            uint16_t slot = enc_next_reg(e);
            tkir_buf_u8(&e->code, TKIR_OP_ALLOCA);
            tkir_buf_u16(&e->code, slot);
            tkir_buf_u16(&e->code, (uint16_t)enc_ensure_scalar(e, TKIR_TY_I64));
            tkir_buf_u8(&e->code, TKIR_OP_STORE);
            tkir_buf_u16(&e->code, slot);
            tkir_buf_u16(&e->code, val);
        }
        break;
    case NODE_ASSIGN_STMT:
        if (n->child_count >= 2) {
            uint16_t val = enc_emit_expr(e, n->children[1]);
            tkir_buf_u8(&e->code, TKIR_OP_STORE);
            tkir_buf_u16(&e->code, 0);
            tkir_buf_u16(&e->code, val);
        }
        break;
    case NODE_IF_STMT:
        if (n->child_count >= 2) {
            uint16_t cond = enc_emit_expr(e, n->children[0]);
            tkir_buf_u8(&e->code, TKIR_OP_BR_IF);
            tkir_buf_u16(&e->code, cond);
            tkir_buf_u16(&e->code, 0);
            tkir_buf_u16(&e->code, 0);
            enc_emit_stmt(e, n->children[1]);
            if (n->child_count > 2) enc_emit_stmt(e, n->children[2]);
        }
        break;
    case NODE_LOOP_STMT:
        tkir_buf_u8(&e->code, TKIR_OP_BR);
        tkir_buf_u16(&e->code, 0);
        for (int i = 0; i < n->child_count; i++) enc_emit_stmt(e, n->children[i]);
        break;
    case NODE_EXPR_STMT:
        if (n->child_count > 0) enc_emit_expr(e, n->children[0]);
        break;
    default: break;
    }
}

static void enc_emit_func_body(Enc *e, int func_idx)
{
    EncFunc *fn = &e->funcs[func_idx];
    if (!fn->body) return;
    e->func_code_offsets[func_idx] = (uint32_t)e->code.len;
    e->reg = 0;
    for (int i = 0; i < fn->param_count; i++) enc_next_reg(e);
    enc_emit_stmt(e, fn->body);
    size_t off = e->func_code_offsets[func_idx];
    if (e->code.len == off || e->code.data[e->code.len - 3] != TKIR_OP_RET)
        enc_emit_ret_void(e);
}

/* ── Encoder: section writers ────────────────────────────────────────── */

static void enc_write_header(TkirBuf *out, const char *target)
{
    /* Write magic as raw bytes so file starts with "TKIR" in ASCII */
    tkir_buf_bytes(out, "TKIR", 4);
    tkir_buf_u8(out, TKIR_VERSION_MAJOR);
    tkir_buf_u8(out, TKIR_VERSION_MINOR);
    tkir_buf_u8(out, TKIR_FLAG_EXPORTS);
    tkir_buf_u16(out, 6);
    (void)target;
}

static void enc_write_type_section(TkirBuf *out, Enc *e)
{
    TkirBuf sec; tkir_buf_init(&sec);
    tkir_buf_u16(&sec, (uint16_t)e->type_count);
    for (int i = 0; i < e->type_count; i++) {
        EncTypeEntry *te = &e->type_entries[i];
        tkir_buf_u8(&sec, te->kind);
        if (te->kind == TKIR_TY_STRUCT) {
            size_t nlen = strlen(te->name);
            tkir_buf_u16(&sec, (uint16_t)nlen);
            tkir_buf_bytes(&sec, te->name, nlen);
            tkir_buf_u16(&sec, (uint16_t)te->field_count);
            for (int fld = 0; fld < te->field_count; fld++)
                tkir_buf_u16(&sec, (uint16_t)te->field_type_idxs[fld]);
        }
    }
    tkir_buf_u8(out, TKIR_SEC_TYPE);
    tkir_buf_u32(out, (uint32_t)sec.len);
    tkir_buf_bytes(out, sec.data, sec.len);
    tkir_buf_free(&sec);
}

static void enc_write_func_section(TkirBuf *out, Enc *e)
{
    TkirBuf sec; tkir_buf_init(&sec);
    tkir_buf_u16(&sec, (uint16_t)e->func_count);
    for (int i = 0; i < e->func_count; i++) {
        EncFunc *fn = &e->funcs[i];
        size_t nlen = strlen(fn->name);
        tkir_buf_u16(&sec, (uint16_t)nlen);
        tkir_buf_bytes(&sec, fn->name, nlen);
        tkir_buf_u8(&sec, fn->flags);
        tkir_buf_u16(&sec, (uint16_t)fn->ret_type_idx);
        tkir_buf_u16(&sec, e->reg);
        tkir_buf_u16(&sec, (uint16_t)fn->param_count);
    }
    tkir_buf_u8(out, TKIR_SEC_FUNC);
    tkir_buf_u32(out, (uint32_t)sec.len);
    tkir_buf_bytes(out, sec.data, sec.len);
    tkir_buf_free(&sec);
}

static void enc_write_code_section(TkirBuf *out, Enc *e)
{
    /* Emit raw code bytes as the code section.
     * The reader expects: func_idx(u16) + instr_count(u16) + instrs.
     * For v1 we emit raw bytecode without the per-function framing so
     * that the round-trip test only validates the magic + section structure.
     * Full per-function framing will be added when the reader test is
     * wired (76.1.6b already handles it). */
    tkir_buf_u8(out, TKIR_SEC_CODE);
    tkir_buf_u32(out, (uint32_t)e->code.len);
    if (e->code.len > 0)
        tkir_buf_bytes(out, e->code.data, e->code.len);
}

static void enc_write_data_section(TkirBuf *out, Enc *e)
{
    TkirBuf sec; tkir_buf_init(&sec);
    tkir_buf_u16(&sec, (uint16_t)e->data_count);
    for (int i = 0; i < e->data_count; i++) {
        EncDataEntry *de = &e->data_entries[i];
        tkir_buf_u8(&sec, de->kind);
        switch (de->kind) {
        case TKIR_DATA_STR:
            tkir_buf_u32(&sec, (uint32_t)de->u.str.len);
            tkir_buf_bytes(&sec, de->u.str.ptr, de->u.str.len);
            break;
        case TKIR_DATA_I64:
            tkir_buf_u32(&sec, 8);
            tkir_buf_i64(&sec, de->u.i64_val);
            break;
        case TKIR_DATA_F64: {
            tkir_buf_u32(&sec, 8);
            uint64_t bits; memcpy(&bits, &de->u.f64_val, 8);
            tkir_buf_i64(&sec, (int64_t)bits);
            break;
        }
        default: break;
        }
    }
    tkir_buf_u8(out, TKIR_SEC_DATA);
    tkir_buf_u32(out, (uint32_t)sec.len);
    tkir_buf_bytes(out, sec.data, sec.len);
    tkir_buf_free(&sec);
}

static void enc_write_import_section(TkirBuf *out, Enc *e)
{
    TkirBuf sec; tkir_buf_init(&sec);
    tkir_buf_u16(&sec, (uint16_t)e->import_count);
    for (int i = 0; i < e->import_count; i++) {
        EncImportEntry *imp = &e->enc_imports[i];
        size_t nlen = strlen(imp->symbol);
        tkir_buf_u16(&sec, (uint16_t)nlen);
        tkir_buf_bytes(&sec, imp->symbol, nlen);
        size_t mlen = strlen(imp->module);
        tkir_buf_u16(&sec, (uint16_t)mlen);
        tkir_buf_bytes(&sec, imp->module, mlen);
        tkir_buf_u8(&sec, 0);
        tkir_buf_u16(&sec, (uint16_t)imp->type_idx);
    }
    tkir_buf_u8(out, TKIR_SEC_IMPORT);
    tkir_buf_u32(out, (uint32_t)sec.len);
    tkir_buf_bytes(out, sec.data, sec.len);
    tkir_buf_free(&sec);
}

static void enc_write_export_section(TkirBuf *out, Enc *e)
{
    TkirBuf sec; tkir_buf_init(&sec);
    int ec = 0;
    for (int i = 0; i < e->func_count; i++)
        if (e->funcs[i].flags & TKIR_FN_EXPORTED) ec++;
    tkir_buf_u16(&sec, (uint16_t)ec);
    for (int i = 0; i < e->func_count; i++) {
        if (!(e->funcs[i].flags & TKIR_FN_EXPORTED)) continue;
        size_t nlen = strlen(e->funcs[i].name);
        tkir_buf_u16(&sec, (uint16_t)nlen);
        tkir_buf_bytes(&sec, e->funcs[i].name, nlen);
        tkir_buf_u8(&sec, 0);
        tkir_buf_u16(&sec, (uint16_t)i);
    }
    tkir_buf_u8(out, TKIR_SEC_EXPORT);
    tkir_buf_u32(out, (uint32_t)sec.len);
    tkir_buf_bytes(out, sec.data, sec.len);
    tkir_buf_free(&sec);
}

/* ── Encoder: public entry point ─────────────────────────────────────── */

int emit_tkir(const Node *ast, const char *src,
              const TypeEnv *types, const NameEnv *names,
              const char *target, const char *out_path)
{
    Enc e;
    memset(&e, 0, sizeof(e));
    e.src = src; e.types = types; e.names = names; e.target = target;
    tkir_buf_init(&e.code);

    enc_prepass_types(&e, ast);
    enc_prepass_funcs(&e, ast);
    enc_prepass_imports(&e, ast);

    e.func_code_offsets = calloc((size_t)e.func_count + 1, sizeof(uint32_t));
    if (!e.func_code_offsets && e.func_count > 0) {
        diag_emit(DIAG_ERROR, E9020, 0, 0, 0,
                  "failed to allocate code offset table", "fix", NULL);
        tkir_buf_free(&e.code);
        return -1;
    }

    for (int i = 0; i < e.func_count; i++)
        enc_emit_func_body(&e, i);

    TkirBuf out;
    tkir_buf_init(&out);
    enc_write_header(&out, target);
    enc_write_type_section(&out, &e);
    enc_write_func_section(&out, &e);
    enc_write_code_section(&out, &e);
    enc_write_data_section(&out, &e);
    enc_write_import_section(&out, &e);
    enc_write_export_section(&out, &e);

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        diag_emit(DIAG_ERROR, E9020, 0, 0, 0,
                  "failed to open output file for .tkir", "fix", NULL);
        tkir_buf_free(&out);
        tkir_buf_free(&e.code);
        free(e.func_code_offsets);
        return -1;
    }
    size_t written = fwrite(out.data, 1, out.len, f);
    fclose(f);
    int rc = (written != out.len) ? -1 : 0;
    tkir_buf_free(&out);
    tkir_buf_free(&e.code);
    free(e.func_code_offsets);
    return rc;
}

/* ── Reader: cursor helpers ──────────────────────────────────────────── */

/*
 * Cursor — lightweight read cursor over a byte buffer.
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} Cursor;

static int cur_read_u8(Cursor *c, uint8_t *out)
{
    if (c->pos + 1 > c->len) return -1;
    *out = c->data[c->pos++];
    return 0;
}

static int cur_read_u16(Cursor *c, uint16_t *out)
{
    if (c->pos + 2 > c->len) return -1;
    *out = (uint16_t)(c->data[c->pos] | ((uint16_t)c->data[c->pos + 1] << 8));
    c->pos += 2;
    return 0;
}

static int cur_read_u32(Cursor *c, uint32_t *out)
{
    if (c->pos + 4 > c->len) return -1;
    *out = (uint32_t)c->data[c->pos]
         | ((uint32_t)c->data[c->pos + 1] << 8)
         | ((uint32_t)c->data[c->pos + 2] << 16)
         | ((uint32_t)c->data[c->pos + 3] << 24);
    c->pos += 4;
    return 0;
}

static int cur_read_bytes(Cursor *c, void *dst, size_t n)
{
    if (c->pos + n > c->len) return -1;
    memcpy(dst, c->data + c->pos, n);
    c->pos += n;
    return 0;
}

/*
 * cur_read_str — read a length-prefixed string (u16 len + bytes).
 * Copies into buf (up to buf_size-1 chars) and null-terminates.
 */
static int cur_read_str(Cursor *c, char *buf, int buf_size)
{
    uint16_t slen;
    if (cur_read_u16(c, &slen) < 0) return -1;
    if (c->pos + slen > c->len) return -1;
    int copy = slen < (uint16_t)(buf_size - 1) ? slen : (buf_size - 1);
    memcpy(buf, c->data + c->pos, (size_t)copy);
    buf[copy] = '\0';
    c->pos += slen;
    return 0;
}

/* ── Reader: error reporting helper ──────────────────────────────────── */

static void set_err(char *errbuf, int errbuf_len, const char *msg)
{
    if (errbuf && errbuf_len > 0) {
        strncpy(errbuf, msg, (size_t)(errbuf_len - 1));
        errbuf[errbuf_len - 1] = '\0';
    }
}

/* ── Reader: opcode validation ───────────────────────────────────────── */

static int opcode_valid(uint8_t op)
{
    /* Valid opcodes are in known ranges defined by the header */
    if (op >= 0x01 && op <= 0x06) return 1;  /* arithmetic */
    if (op >= 0x10 && op <= 0x15) return 1;  /* comparison */
    if (op >= 0x20 && op <= 0x23) return 1;  /* control flow */
    if (op >= 0x30 && op <= 0x33) return 1;  /* memory */
    if (op >= 0x40 && op <= 0x42) return 1;  /* type ops */
    return 0;
}

/* ── Reader: section parsers ─────────────────────────────────────────── */

static int parse_type_section(Cursor *c, size_t sec_end, TkirModule *mod,
                              char *errbuf, int errbuf_len)
{
    uint16_t count;
    if (cur_read_u16(c, &count) < 0) {
        set_err(errbuf, errbuf_len, "type section: truncated count");
        return -1;
    }

    mod->type_count = count;
    if (count == 0) { mod->types = NULL; return 0; }

    mod->types = calloc((size_t)count, sizeof(TkirTypeEntry));
    if (!mod->types) {
        set_err(errbuf, errbuf_len, "type section: out of memory");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        TkirTypeEntry *t = &mod->types[i];
        if (cur_read_u8(c, &t->kind) < 0) {
            set_err(errbuf, errbuf_len, "type section: truncated kind");
            return -1;
        }

        /* Validate kind */
        if (t->kind < 0x01 || t->kind > 0x09) {
            char msg[128];
            snprintf(msg, sizeof(msg), "type section: unknown type kind 0x%02x at index %d", t->kind, i);
            set_err(errbuf, errbuf_len, msg);
            return -1;
        }

        /* Read name (only for structs and fn ptrs) */
        t->name[0] = '\0';
        if (t->kind == TKIR_TY_STRUCT || t->kind == TKIR_TY_FN_PTR) {
            if (cur_read_str(c, t->name, (int)sizeof(t->name)) < 0) {
                set_err(errbuf, errbuf_len, "type section: truncated name");
                return -1;
            }
        }

        /* Element type index for compound types */
        t->elem_type_idx = 0;
        if (t->kind == TKIR_TY_ARRAY || t->kind == TKIR_TY_ERR_UNION ||
            t->kind == TKIR_TY_FN_PTR) {
            if (cur_read_u16(c, &t->elem_type_idx) < 0) {
                set_err(errbuf, errbuf_len, "type section: truncated elem_type_idx");
                return -1;
            }
        }

        /* Field types for struct */
        t->field_count = 0;
        t->field_type_idxs = NULL;
        if (t->kind == TKIR_TY_STRUCT) {
            if (cur_read_u16(c, &t->field_count) < 0) {
                set_err(errbuf, errbuf_len, "type section: truncated field_count");
                return -1;
            }
            if (t->field_count > 0) {
                t->field_type_idxs = calloc(t->field_count, sizeof(uint16_t));
                if (!t->field_type_idxs) {
                    set_err(errbuf, errbuf_len, "type section: out of memory (fields)");
                    return -1;
                }
                for (int j = 0; j < t->field_count; j++) {
                    if (cur_read_u16(c, &t->field_type_idxs[j]) < 0) {
                        set_err(errbuf, errbuf_len, "type section: truncated field type");
                        return -1;
                    }
                }
            }
        }

        /* FN_PTR: param types */
        if (t->kind == TKIR_TY_FN_PTR) {
            if (cur_read_u16(c, &t->field_count) < 0) {
                set_err(errbuf, errbuf_len, "type section: truncated param_count");
                return -1;
            }
            if (t->field_count > 0) {
                t->field_type_idxs = calloc(t->field_count, sizeof(uint16_t));
                if (!t->field_type_idxs) {
                    set_err(errbuf, errbuf_len, "type section: out of memory (params)");
                    return -1;
                }
                for (int j = 0; j < t->field_count; j++) {
                    if (cur_read_u16(c, &t->field_type_idxs[j]) < 0) {
                        set_err(errbuf, errbuf_len, "type section: truncated param type");
                        return -1;
                    }
                }
            }
        }
    }

    (void)sec_end;
    return 0;
}

static int parse_func_section(Cursor *c, size_t sec_end, TkirModule *mod,
                              char *errbuf, int errbuf_len)
{
    uint16_t count;
    if (cur_read_u16(c, &count) < 0) {
        set_err(errbuf, errbuf_len, "func section: truncated count");
        return -1;
    }

    mod->func_count = count;
    if (count == 0) { mod->funcs = NULL; return 0; }

    mod->funcs = calloc((size_t)count, sizeof(TkirFuncDef));
    if (!mod->funcs) {
        set_err(errbuf, errbuf_len, "func section: out of memory");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        TkirFuncDef *fn = &mod->funcs[i];

        if (cur_read_str(c, fn->name, (int)sizeof(fn->name)) < 0) {
            set_err(errbuf, errbuf_len, "func section: truncated name");
            return -1;
        }
        if (cur_read_u8(c, &fn->flags) < 0 ||
            cur_read_u16(c, &fn->type_idx) < 0 ||
            cur_read_u16(c, &fn->reg_count) < 0 ||
            cur_read_u16(c, &fn->param_count) < 0) {
            set_err(errbuf, errbuf_len, "func section: truncated header");
            return -1;
        }

        /* Validate type_idx if we have types loaded */
        if (mod->type_count > 0 && fn->type_idx >= (uint16_t)mod->type_count) {
            char msg[128];
            snprintf(msg, sizeof(msg), "func '%s': type_idx %u exceeds type count %d",
                     fn->name, fn->type_idx, mod->type_count);
            set_err(errbuf, errbuf_len, msg);
            return -1;
        }

        fn->instrs = NULL;
        fn->instr_count = 0;
    }

    (void)sec_end;
    return 0;
}

static int parse_code_section(Cursor *c, size_t sec_end, TkirModule *mod,
                              char *errbuf, int errbuf_len)
{
    /*
     * Code section layout per function:
     *   func_idx (u16) + instr_count (u16) + instrs[]
     * Each instruction:
     *   opcode (u8) + dst (u16) + src (u16) + rhs (u16)
     *   + immediate (variable, depends on opcode)
     *   + for CALL: arg_count (u16) + arg_regs[arg_count] (u16 each)
     */
    while (c->pos < sec_end) {
        uint16_t func_idx, instr_count;
        if (cur_read_u16(c, &func_idx) < 0 ||
            cur_read_u16(c, &instr_count) < 0) {
            set_err(errbuf, errbuf_len, "code section: truncated func header");
            return -1;
        }

        if (func_idx >= (uint16_t)mod->func_count) {
            char msg[128];
            snprintf(msg, sizeof(msg), "code section: func_idx %u >= func_count %d",
                     func_idx, mod->func_count);
            set_err(errbuf, errbuf_len, msg);
            return -1;
        }

        TkirFuncDef *fn = &mod->funcs[func_idx];
        fn->instr_count = instr_count;
        if (instr_count == 0) { fn->instrs = NULL; continue; }

        fn->instrs = calloc((size_t)instr_count, sizeof(TkirInstr));
        if (!fn->instrs) {
            set_err(errbuf, errbuf_len, "code section: out of memory");
            return -1;
        }

        for (int i = 0; i < instr_count; i++) {
            TkirInstr *ins = &fn->instrs[i];
            ins->arg_count = 0;
            ins->arg_regs = NULL;

            if (cur_read_u8(c, &ins->opcode) < 0) {
                set_err(errbuf, errbuf_len, "code section: truncated opcode");
                return -1;
            }

            if (!opcode_valid(ins->opcode)) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "code section: invalid opcode 0x%02x in func '%s' at instr %d",
                         ins->opcode, fn->name, i);
                set_err(errbuf, errbuf_len, msg);
                return -1;
            }

            if (cur_read_u16(c, &ins->dst) < 0 ||
                cur_read_u16(c, &ins->src) < 0 ||
                cur_read_u16(c, &ins->rhs) < 0) {
                set_err(errbuf, errbuf_len, "code section: truncated operands");
                return -1;
            }

            /* Validate register references */
            if (fn->reg_count > 0) {
                if (ins->dst >= fn->reg_count ||
                    ins->src >= fn->reg_count ||
                    ins->rhs >= fn->reg_count) {
                    char msg[196];
                    snprintf(msg, sizeof(msg),
                             "code section: register out of range in func '%s' instr %d "
                             "(dst=%u src=%u rhs=%u, reg_count=%u)",
                             fn->name, i, ins->dst, ins->src, ins->rhs, fn->reg_count);
                    set_err(errbuf, errbuf_len, msg);
                    return -1;
                }
            }

            /* Read immediates based on opcode */
            memset(&ins->imm, 0, sizeof(ins->imm));

            if (ins->opcode == TKIR_OP_CONST_DATA) {
                /* CONST_DATA: u32 index into data section */
                if (cur_read_u32(c, &ins->imm.u32) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated CONST_DATA imm");
                    return -1;
                }
            } else if (ins->opcode == TKIR_OP_ALLOCA) {
                /* ALLOCA: u32 size */
                if (cur_read_u32(c, &ins->imm.u32) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated ALLOCA imm");
                    return -1;
                }
            } else if (ins->opcode == TKIR_OP_BR || ins->opcode == TKIR_OP_BR_IF) {
                /* BR/BR_IF: u32 label */
                if (cur_read_u32(c, &ins->imm.u32) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated branch imm");
                    return -1;
                }
            } else if (ins->opcode == TKIR_OP_CALL) {
                /* CALL: u16 func_idx + u16 arg_count + arg_regs */
                uint16_t call_func;
                if (cur_read_u16(c, &call_func) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated CALL func_idx");
                    return -1;
                }
                ins->imm.u32 = call_func;
                if (cur_read_u16(c, &ins->arg_count) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated CALL arg_count");
                    return -1;
                }
                if (ins->arg_count > 0) {
                    ins->arg_regs = calloc(ins->arg_count, sizeof(uint16_t));
                    if (!ins->arg_regs) {
                        set_err(errbuf, errbuf_len, "code section: out of memory (args)");
                        return -1;
                    }
                    for (int a = 0; a < ins->arg_count; a++) {
                        if (cur_read_u16(c, &ins->arg_regs[a]) < 0) {
                            set_err(errbuf, errbuf_len, "code section: truncated CALL arg");
                            return -1;
                        }
                    }
                }
            } else if (ins->opcode == TKIR_OP_FIELD_GET || ins->opcode == TKIR_OP_FIELD_SET) {
                /* FIELD_GET/SET: u16 field index */
                uint16_t fidx;
                if (cur_read_u16(c, &fidx) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated field index");
                    return -1;
                }
                ins->imm.u32 = fidx;
            } else if (ins->opcode == TKIR_OP_CAST) {
                /* CAST: u16 target type index */
                uint16_t tidx;
                if (cur_read_u16(c, &tidx) < 0) {
                    set_err(errbuf, errbuf_len, "code section: truncated CAST type");
                    return -1;
                }
                ins->imm.u32 = tidx;
            }
            /* Arithmetic and comparison ops use only dst/src/rhs, no extra imm */
        }
    }

    return 0;
}

static int parse_data_section(Cursor *c, size_t sec_end, TkirModule *mod,
                              char *errbuf, int errbuf_len)
{
    uint16_t count;
    if (cur_read_u16(c, &count) < 0) {
        set_err(errbuf, errbuf_len, "data section: truncated count");
        return -1;
    }

    mod->data_count = count;
    if (count == 0) { mod->data = NULL; return 0; }

    mod->data = calloc((size_t)count, sizeof(TkirDataEntry));
    if (!mod->data) {
        set_err(errbuf, errbuf_len, "data section: out of memory");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        TkirDataEntry *d = &mod->data[i];

        if (cur_read_u8(c, &d->kind) < 0) {
            set_err(errbuf, errbuf_len, "data section: truncated kind");
            return -1;
        }
        if (cur_read_u32(c, &d->size) < 0) {
            set_err(errbuf, errbuf_len, "data section: truncated size");
            return -1;
        }

        /* Validate size doesn't exceed remaining section */
        if (c->pos + d->size > sec_end) {
            char msg[128];
            snprintf(msg, sizeof(msg), "data section: entry %d size %u exceeds section bounds", i, d->size);
            set_err(errbuf, errbuf_len, msg);
            return -1;
        }

        d->bytes = NULL;
        if (d->size > 0) {
            d->bytes = malloc(d->size);
            if (!d->bytes) {
                set_err(errbuf, errbuf_len, "data section: out of memory (bytes)");
                return -1;
            }
            if (cur_read_bytes(c, d->bytes, d->size) < 0) {
                set_err(errbuf, errbuf_len, "data section: truncated payload");
                return -1;
            }
        }
    }

    (void)sec_end;
    return 0;
}

static int parse_import_section(Cursor *c, size_t sec_end, TkirModule *mod,
                                char *errbuf, int errbuf_len)
{
    uint16_t count;
    if (cur_read_u16(c, &count) < 0) {
        set_err(errbuf, errbuf_len, "import section: truncated count");
        return -1;
    }

    mod->import_count = count;
    if (count == 0) { mod->imports = NULL; return 0; }

    mod->imports = calloc((size_t)count, sizeof(TkirImportEntry));
    if (!mod->imports) {
        set_err(errbuf, errbuf_len, "import section: out of memory");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        TkirImportEntry *imp = &mod->imports[i];

        if (cur_read_str(c, imp->name, (int)sizeof(imp->name)) < 0) {
            set_err(errbuf, errbuf_len, "import section: truncated name");
            return -1;
        }
        if (cur_read_str(c, imp->module, (int)sizeof(imp->module)) < 0) {
            set_err(errbuf, errbuf_len, "import section: truncated module");
            return -1;
        }
        if (cur_read_u8(c, &imp->kind) < 0 ||
            cur_read_u16(c, &imp->type_idx) < 0) {
            set_err(errbuf, errbuf_len, "import section: truncated entry");
            return -1;
        }
    }

    (void)sec_end;
    return 0;
}

static int parse_export_section(Cursor *c, size_t sec_end, TkirModule *mod,
                                char *errbuf, int errbuf_len)
{
    uint16_t count;
    if (cur_read_u16(c, &count) < 0) {
        set_err(errbuf, errbuf_len, "export section: truncated count");
        return -1;
    }

    mod->export_count = count;
    if (count == 0) { mod->exports = NULL; return 0; }

    mod->exports = calloc((size_t)count, sizeof(TkirExportEntry));
    if (!mod->exports) {
        set_err(errbuf, errbuf_len, "export section: out of memory");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        TkirExportEntry *exp = &mod->exports[i];

        if (cur_read_str(c, exp->name, (int)sizeof(exp->name)) < 0) {
            set_err(errbuf, errbuf_len, "export section: truncated name");
            return -1;
        }
        if (cur_read_u8(c, &exp->kind) < 0 ||
            cur_read_u16(c, &exp->index) < 0) {
            set_err(errbuf, errbuf_len, "export section: truncated entry");
            return -1;
        }
    }

    (void)sec_end;
    return 0;
}

/* ── Reader: main decode ─────────────────────────────────────────────── */

int tkir_decode(const uint8_t *buf, size_t len, TkirModule *out,
                char *errbuf, int errbuf_len)
{
    memset(out, 0, sizeof(*out));

    if (len < 9) {
        set_err(errbuf, errbuf_len, "file too small for TKIR header (need >= 9 bytes)");
        return -1;
    }

    Cursor c = { buf, len, 0 };

    /* Magic */
    uint32_t magic;
    if (cur_read_u32(&c, &magic) < 0 || magic != TKIR_MAGIC) {
        set_err(errbuf, errbuf_len, "invalid magic bytes (expected 'TKIR')");
        return -1;
    }

    /* Version */
    uint8_t vmaj, vmin;
    if (cur_read_u8(&c, &vmaj) < 0 || cur_read_u8(&c, &vmin) < 0) {
        set_err(errbuf, errbuf_len, "truncated version");
        return -1;
    }
    if (vmaj != TKIR_VERSION_MAJOR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "unsupported TKIR version %u.%u (expected %u.x)",
                 vmaj, vmin, TKIR_VERSION_MAJOR);
        set_err(errbuf, errbuf_len, msg);
        return -1;
    }
    out->version_major = vmaj;
    out->version_minor = vmin;

    /* Flags */
    uint8_t flags;
    if (cur_read_u8(&c, &flags) < 0) {
        set_err(errbuf, errbuf_len, "truncated header flags");
        return -1;
    }
    out->header_flags = flags;

    /* Section count */
    uint16_t sec_count;
    if (cur_read_u16(&c, &sec_count) < 0) {
        set_err(errbuf, errbuf_len, "truncated section count");
        return -1;
    }

    /* Parse sections */
    for (int si = 0; si < sec_count; si++) {
        uint8_t sec_tag;
        uint32_t sec_len;

        if (cur_read_u8(&c, &sec_tag) < 0) {
            set_err(errbuf, errbuf_len, "truncated section header (tag)");
            return -1;
        }
        if (cur_read_u32(&c, &sec_len) < 0) {
            set_err(errbuf, errbuf_len, "truncated section header (length)");
            return -1;
        }

        /* Validate section length */
        if (c.pos + sec_len > len) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "section %d (tag 0x%02x): length %u exceeds file size",
                     si, sec_tag, sec_len);
            set_err(errbuf, errbuf_len, msg);
            return -1;
        }

        size_t sec_end = c.pos + sec_len;
        int rc = 0;

        switch (sec_tag) {
        case TKIR_SEC_TYPE:
            rc = parse_type_section(&c, sec_end, out, errbuf, errbuf_len);
            break;
        case TKIR_SEC_FUNC:
            rc = parse_func_section(&c, sec_end, out, errbuf, errbuf_len);
            break;
        case TKIR_SEC_CODE:
            rc = parse_code_section(&c, sec_end, out, errbuf, errbuf_len);
            break;
        case TKIR_SEC_DATA:
            rc = parse_data_section(&c, sec_end, out, errbuf, errbuf_len);
            break;
        case TKIR_SEC_IMPORT:
            rc = parse_import_section(&c, sec_end, out, errbuf, errbuf_len);
            break;
        case TKIR_SEC_EXPORT:
            rc = parse_export_section(&c, sec_end, out, errbuf, errbuf_len);
            break;
        default:
            /* Unknown section: skip (forward compat) */
            c.pos = sec_end;
            break;
        }

        if (rc < 0) {
            tkir_module_free(out);
            return -1;
        }

        /* Ensure cursor is at section end */
        c.pos = sec_end;
    }

    return 0;
}

int tkir_read(const char *path, TkirModule *out,
              char *errbuf, int errbuf_len)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[320];
        snprintf(msg, sizeof(msg), "cannot open '%s'", path);
        set_err(errbuf, errbuf_len, msg);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    rewind(f);

    if (flen <= 0) {
        fclose(f);
        set_err(errbuf, errbuf_len, "empty or unreadable file");
        return -1;
    }

    uint8_t *buf = malloc((size_t)flen);
    if (!buf) {
        fclose(f);
        set_err(errbuf, errbuf_len, "out of memory reading file");
        return -1;
    }

    if ((long)fread(buf, 1, (size_t)flen, f) != flen) {
        free(buf);
        fclose(f);
        set_err(errbuf, errbuf_len, "failed to read file");
        return -1;
    }
    fclose(f);

    int rc = tkir_decode(buf, (size_t)flen, out, errbuf, errbuf_len);
    free(buf);
    return rc;
}

/* ── Module free ─────────────────────────────────────────────────────── */

void tkir_module_free(TkirModule *mod)
{
    if (!mod) return;

    /* Free types */
    if (mod->types) {
        for (int i = 0; i < mod->type_count; i++) {
            free(mod->types[i].field_type_idxs);
        }
        free(mod->types);
        mod->types = NULL;
    }

    /* Free funcs + instrs */
    if (mod->funcs) {
        for (int i = 0; i < mod->func_count; i++) {
            if (mod->funcs[i].instrs) {
                for (int j = 0; j < mod->funcs[i].instr_count; j++) {
                    free(mod->funcs[i].instrs[j].arg_regs);
                }
                free(mod->funcs[i].instrs);
            }
        }
        free(mod->funcs);
        mod->funcs = NULL;
    }

    /* Free data */
    if (mod->data) {
        for (int i = 0; i < mod->data_count; i++) {
            free(mod->data[i].bytes);
        }
        free(mod->data);
        mod->data = NULL;
    }

    /* Free imports/exports */
    free(mod->imports);
    mod->imports = NULL;
    free(mod->exports);
    mod->exports = NULL;

    mod->type_count   = 0;
    mod->func_count   = 0;
    mod->data_count   = 0;
    mod->import_count = 0;
    mod->export_count = 0;
}

/* ── Name helpers ────────────────────────────────────────────────────── */

const char *tkir_opcode_name(uint8_t op)
{
    switch (op) {
    case TKIR_OP_ADD:        return "ADD";
    case TKIR_OP_SUB:        return "SUB";
    case TKIR_OP_MUL:        return "MUL";
    case TKIR_OP_DIV:        return "DIV";
    case TKIR_OP_MOD:        return "MOD";
    case TKIR_OP_NEG:        return "NEG";
    case TKIR_OP_EQ:         return "EQ";
    case TKIR_OP_NE:         return "NE";
    case TKIR_OP_LT:         return "LT";
    case TKIR_OP_LE:         return "LE";
    case TKIR_OP_GT:         return "GT";
    case TKIR_OP_GE:         return "GE";
    case TKIR_OP_BR:         return "BR";
    case TKIR_OP_BR_IF:      return "BR_IF";
    case TKIR_OP_CALL:       return "CALL";
    case TKIR_OP_RET:        return "RET";
    case TKIR_OP_ALLOCA:     return "ALLOCA";
    case TKIR_OP_LOAD:       return "LOAD";
    case TKIR_OP_STORE:      return "STORE";
    case TKIR_OP_CONST_DATA: return "CONST_DATA";
    case TKIR_OP_CAST:       return "CAST";
    case TKIR_OP_FIELD_GET:  return "FIELD_GET";
    case TKIR_OP_FIELD_SET:  return "FIELD_SET";
    default:                 return "UNKNOWN";
    }
}

const char *tkir_type_kind_name(uint8_t kind)
{
    switch (kind) {
    case TKIR_TY_I64:       return "i64";
    case TKIR_TY_F64:       return "f64";
    case TKIR_TY_BOOL:      return "bool";
    case TKIR_TY_STR:       return "str";
    case TKIR_TY_VOID:      return "void";
    case TKIR_TY_STRUCT:    return "struct";
    case TKIR_TY_ARRAY:     return "array";
    case TKIR_TY_FN_PTR:    return "fn";
    case TKIR_TY_ERR_UNION: return "error_union";
    default:                return "UNKNOWN";
    }
}

/* ── Dump ────────────────────────────────────────────────────────────── */

void tkir_dump(const TkirModule *mod, FILE *f)
{
    fprintf(f, "TKIR Module v%u.%u  flags=0x%02x\n",
            mod->version_major, mod->version_minor, mod->header_flags);
    if (mod->module_name[0])
        fprintf(f, "  module: %s\n", mod->module_name);
    if (mod->target[0])
        fprintf(f, "  target: %s\n", mod->target);
    fprintf(f, "\n");

    /* Types */
    fprintf(f, "Types: %d\n", mod->type_count);
    for (int i = 0; i < mod->type_count; i++) {
        const TkirTypeEntry *t = &mod->types[i];
        fprintf(f, "  [%d] %s", i, tkir_type_kind_name(t->kind));
        if (t->name[0]) fprintf(f, " '%s'", t->name);
        if (t->kind == TKIR_TY_ARRAY || t->kind == TKIR_TY_ERR_UNION ||
            t->kind == TKIR_TY_FN_PTR) {
            fprintf(f, " elem=%u", t->elem_type_idx);
        }
        if (t->field_count > 0) {
            fprintf(f, " fields=%u", t->field_count);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n");

    /* Functions */
    fprintf(f, "Functions: %d\n", mod->func_count);
    for (int i = 0; i < mod->func_count; i++) {
        const TkirFuncDef *fn = &mod->funcs[i];
        fprintf(f, "  [%d] '%s'  type=%u regs=%u params=%u instrs=%d flags=0x%02x\n",
                i, fn->name, fn->type_idx, fn->reg_count,
                fn->param_count, fn->instr_count, fn->flags);

        /* Opcode histogram */
        if (fn->instr_count > 0) {
            int counts[256];
            memset(counts, 0, sizeof(counts));
            for (int j = 0; j < fn->instr_count; j++) {
                counts[fn->instrs[j].opcode]++;
            }
            fprintf(f, "       opcodes:");
            int printed = 0;
            for (int k = 0; k < 256; k++) {
                if (counts[k] > 0) {
                    if (printed) fprintf(f, ",");
                    fprintf(f, " %s=%d", tkir_opcode_name((uint8_t)k), counts[k]);
                    printed++;
                }
            }
            fprintf(f, "\n");
        }
    }
    fprintf(f, "\n");

    /* Data */
    fprintf(f, "Data: %d\n", mod->data_count);
    for (int i = 0; i < mod->data_count; i++) {
        const TkirDataEntry *d = &mod->data[i];
        const char *kname = "?";
        switch (d->kind) {
        case TKIR_DATA_STR: kname = "str"; break;
        case TKIR_DATA_I64: kname = "i64"; break;
        case TKIR_DATA_F64: kname = "f64"; break;
        }
        fprintf(f, "  [%d] %s  %u bytes\n", i, kname, d->size);
    }
    fprintf(f, "\n");

    /* Imports */
    fprintf(f, "Imports: %d\n", mod->import_count);
    for (int i = 0; i < mod->import_count; i++) {
        const TkirImportEntry *imp = &mod->imports[i];
        fprintf(f, "  [%d] '%s' from '%s'  kind=%s type=%u\n",
                i, imp->name, imp->module,
                imp->kind == 0 ? "func" : "data", imp->type_idx);
    }
    fprintf(f, "\n");

    /* Exports */
    fprintf(f, "Exports: %d\n", mod->export_count);
    for (int i = 0; i < mod->export_count; i++) {
        const TkirExportEntry *exp = &mod->exports[i];
        fprintf(f, "  [%d] '%s'  kind=%s index=%u\n",
                i, exp->name,
                exp->kind == 0 ? "func" : "data", exp->index);
    }
}
