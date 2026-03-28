/* llvm.c — LLVM IR text emitter for toke. Story: 1.2.8 */
#include "llvm.h"
#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { FILE *out; const char *src; int tmp, str_idx, lbl; } Ctx;
static int next_tmp(Ctx *c){return c->tmp++;} static int next_lbl(Ctx *c){return c->lbl++;} static int next_str(Ctx *c){return c->str_idx++;}
static void tok_cp(const char *src,const Node *n,char *buf,int sz){int len=n->tok_len<sz-1?n->tok_len:sz-1;memcpy(buf,src+n->tok_start,(size_t)len);buf[len]='\0';}

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
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = call i64 @%s(", t, tb);
        for (int i = 0; i < na; i++) {
            if (i) fputc(',', c->out);
            fprintf(c->out, " i64 %%t%d", args[i]);
        }
        fputs(")\n", c->out);
        return t;
    }
    case NODE_CAST_EXPR: {
        int v = emit_expr(c, n->children[0]);
        t = next_tmp(c);
        fprintf(c->out, "  ; as-cast — identity stub\n");
        fprintf(c->out, "  %%t%d = add i64 0, %%t%d\n", t, v);
        return t;
    }
    case NODE_FIELD_EXPR: {
        char fn[128]; tok_cp(c->src, n->children[1], fn, sizeof fn);
        int base = emit_expr(c, n->children[0]);
        t2 = next_tmp(c); t = next_tmp(c);
        fprintf(c->out, "  %%t%d = getelementptr i64, ptr %%t%d, i32 0 ; .%s\n", t2, base, fn);
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
        t = next_tmp(c);
        fprintf(c->out, "  %%t%d = alloca i64 ; struct_lit\n", t);
        for (int i = 0; i < n->child_count; i++) {
            const Node *fi = n->children[i];
            if (fi->child_count >= 2) {
                t3 = emit_expr(c, fi->children[1]);
                fprintf(c->out, "  store i64 %%t%d, ptr %%t%d ; field %d\n", t3, t, i);
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

/* ── Statement emission ────────────────────────────────────────────── */

static void emit_stmt(Ctx *c, const Node *n)
{
    char tb[256];
    switch (n->kind) {
    case NODE_BIND_STMT:
    case NODE_MUT_BIND_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        fprintf(c->out, "  %%%s = alloca i64\n", tb);
        if (n->child_count >= 3) {
            int v = emit_expr(c, n->children[2]);
            fprintf(c->out, "  store i64 %%t%d, ptr %%%s\n", v, tb);
        }
        break;
    case NODE_ASSIGN_STMT:
        tok_cp(c->src, n->children[0], tb, sizeof tb);
        { int v = emit_expr(c, n->children[1]);
          fprintf(c->out, "  store i64 %%t%d, ptr %%%s\n", v, tb); }
        break;
    case NODE_RETURN_STMT:
        if (n->child_count > 0) {
            int v = emit_expr(c, n->children[0]);
            fprintf(c->out, "  ret i64 %%t%d\n", v);
        } else {
            fputs("  ret void\n", c->out);
        }
        break;
    case NODE_BREAK_STMT:
        fprintf(c->out, "  br label %%loop_exit%d\n", c->lbl - 1);
        break;
    case NODE_IF_STMT: {
        int L = next_lbl(c);
        int cv = emit_expr(c, n->children[0]);
        fprintf(c->out, "  br i1 %%t%d, label %%if_then%d, label %%if_else%d\n", cv, L, L);
        fprintf(c->out, "if_then%d:\n", L);
        emit_stmt(c, n->children[1]);
        fprintf(c->out, "  br label %%if_merge%d\n", L);
        fprintf(c->out, "if_else%d:\n", L);
        if (n->child_count >= 3) emit_stmt(c, n->children[2]);
        fprintf(c->out, "  br label %%if_merge%d\n", L);
        fprintf(c->out, "if_merge%d:\n", L);
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
        emit_stmt(c, body);
        c->lbl = save;
        fprintf(c->out, "  br label %%loop_hdr%d\n", L);
        fprintf(c->out, "loop_exit%d:\n", L);
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
            emit_stmt(c, arm->children[1]);
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
                if (rs->child_count > 0) {
                    if (rs->children[0]->kind == NODE_PTR_TYPE) {
                        ret = "ptr";
                    } else {
                        char rn[64]; tok_cp(c->src, rs->children[0], rn, sizeof rn);
                        if      (!strcmp(rn, "bool")) ret = "i1";
                        else if (!strcmp(rn, "f64"))  ret = "double";
                        else if (!strcmp(rn, "str"))  ret = "ptr";
                        else if (!strcmp(rn, "void")) ret = "void";
                        else                          ret = "i64";
                    }
                }
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
                /* Determine LLVM type for param */
                const char *pty = "i64";
                if (n->children[i]->child_count > 1 && n->children[i]->children[1]) {
                    const Node *pt = n->children[i]->children[1];
                    if (pt->kind == NODE_PTR_TYPE) {
                        pty = "ptr";
                    } else {
                        char ptn[64]; tok_cp(c->src, pt, ptn, sizeof ptn);
                        if      (!strcmp(ptn, "bool")) pty = "i1";
                        else if (!strcmp(ptn, "f64"))  pty = "double";
                        else if (!strcmp(ptn, "str"))  pty = "ptr";
                    }
                }
                fputs(pty, c->out);
                first = 0;
            }
            fputs(")\n", c->out);
            break;
        }
        fprintf(c->out, "\ndefine %s @%s(", ret, tb);
        int first = 1;
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            if (!first) fputs(", ", c->out);
            fprintf(c->out, "i64 %%%s", pn);
            first = 0;
        }
        fputs(") {\nentry:\n", c->out);
        /* Spill params */
        for (int i = 1; i < n->child_count; i++) {
            if (n->children[i]->kind != NODE_PARAM) continue;
            char pn[128]; tok_cp(c->src, n->children[i]->children[0], pn, sizeof pn);
            fprintf(c->out, "  %%%s.addr = alloca i64\n  store i64 %%%s, ptr %%%s.addr\n", pn, pn, pn);
        }
        if (body_i >= 0) emit_stmt(c, n->children[body_i]);
        if (!strcmp(ret, "void")) fputs("  ret void\n", c->out);
        fputs("}\n", c->out);
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
    Ctx ctx; ctx.out = f; ctx.src = src; ctx.tmp = 0; ctx.str_idx = 0; ctx.lbl = 0;

    fputs("; module generated by tkc\n", f);
    if (env && env->target && env->target[0])
        fprintf(f, "target triple = \"%s\"\n", env->target);
    fputs("\ndeclare i32 @printf(ptr, ...)\ndeclare i32 @puts(ptr)\n", f);
    fputs("declare ptr @tk_spawn(ptr)\ndeclare i64 @tk_await(ptr)\n\n", f);

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
