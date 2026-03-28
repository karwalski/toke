#include "ir.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* ir.c — Interface file emitter (Story 1.2.7). */

/* Convert TypeKind to tki string; writes into buf[size]. Returns buf. */
static const char *type_to_str(const Type *t, char *buf, int size)
{
    if (!t) { snprintf(buf, size, "unknown"); return buf; }
    switch (t->kind) {
        case TY_VOID:  snprintf(buf, size, "void"); break;
        case TY_BOOL:  snprintf(buf, size, "bool"); break;
        case TY_I64:   snprintf(buf, size, "i64");  break;
        case TY_U64:   snprintf(buf, size, "u64");  break;
        case TY_F64:   snprintf(buf, size, "f64");  break;
        case TY_STR:   snprintf(buf, size, "str");  break;
        case TY_STRUCT:
            snprintf(buf, size, "%s", t->name ? t->name : "struct"); break;
        case TY_ARRAY: {
            char e[64]; type_to_str(t->elem, e, sizeof(e));
            snprintf(buf, size, "[%s]", e); break;
        }
        case TY_ERROR_TYPE: {
            char e[64]; type_to_str(t->elem, e, sizeof(e));
            snprintf(buf, size, "%s!err", e); break;
        }
        case TY_FUNC: {
            char e[64]; type_to_str(t->elem, e, sizeof(e));
            snprintf(buf, size, "func->%s", e); break;
        }
        default: snprintf(buf, size, "unknown"); break;
    }
    return buf;
}

/* Write a JSON-escaped string (without surrounding quotes) to fp. */
static void json_str(FILE *fp, const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\n') fputs("\\n",  fp);
        else if (c == '\r') fputs("\\r",  fp);
        else if (c == '\t') fputs("\\t",  fp);
        else if (c < 0x20)  fprintf(fp, "\\u%04x", c);
        else                fputc(c, fp);
    }
}

/* Collect the dotted module path from NODE_MODULE > NODE_MODULE_PATH. */
static void collect_module_path(const Node *ast, const char *src,
                                char *buf, int size)
{
    buf[0] = '\0';
    for (int i = 0; i < ast->child_count; i++) {
        const Node *mod = ast->children[i];
        if (!mod || mod->kind != NODE_MODULE) continue;
        for (int j = 0; j < mod->child_count; j++) {
            const Node *mp = mod->children[j];
            if (!mp || mp->kind != NODE_MODULE_PATH) continue;
            int pos = 0;
            for (int k = 0; k < mp->child_count && pos < size - 1; k++) {
                const Node *id = mp->children[k];
                if (!id) continue;
                if (pos > 0 && pos < size - 2) buf[pos++] = '.';
                int len = id->tok_len < size - 1 - pos ? id->tok_len : size - 1 - pos;
                memcpy(buf + pos, src + id->tok_start, len);
                pos += len;
            }
            buf[pos] = '\0';
            return;
        }
    }
}

/* Copy node token text into buf (NUL-terminated). Returns buf. */
static const char *tok_copy(const Node *n, const char *src, char *buf, int size)
{
    int len = n->tok_len < size - 1 ? n->tok_len : size - 1;
    memcpy(buf, src + n->tok_start, len);
    buf[len] = '\0';
    return buf;
}

int emit_interface(const Node *ast, const char *src,
                   const TypeEnv *type_env, const char *out_path)
{
    (void)type_to_str; (void)type_env;
    FILE *fp = fopen(out_path, "w");
    if (!fp) {
        diag_emit(DIAG_ERROR, E9001, 0, 0, 0,
                  "failed to write interface file '%s': %s",
                  out_path, strerror(errno), (char *)NULL);
        return -1;
    }
    char mod[256]; collect_module_path(ast, src, mod, sizeof(mod));
    fputs("{\n  \"schema_version\": \"1.0\",\n  \"module\": \"", fp);
    json_str(fp, mod, (int)strlen(mod));
    fputs("\",\n  \"exports\": [", fp);

    int first = 1;
    char nbuf[128], tbuf[128];
    for (int i = 0; i < ast->child_count; i++) {
        const Node *top = ast->children[i];
        if (!top) continue;
        if (top->kind == NODE_FUNC_DECL) {
            if (top->child_count < 1 || !top->children[0]) continue;
            tok_copy(top->children[0], src, nbuf, sizeof(nbuf));
            if (!first) fputs(",", fp); first = 0;
            fputs("\n    {\"kind\": \"func\", \"name\": \"", fp);
            json_str(fp, nbuf, (int)strlen(nbuf));
            fputs("\", \"params\": [", fp);
            int fp2 = 1;
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_PARAM || ch->child_count < 2 || !ch->children[1]) continue;
                tok_copy(ch->children[1], src, tbuf, sizeof(tbuf));
                if (!fp2) fputs(", ", fp); fp2 = 0;
                fputc('"', fp); json_str(fp, tbuf, (int)strlen(tbuf)); fputc('"', fp);
            }
            fputs("], \"return\": \"", fp);
            const char *ret = "void";
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_RETURN_SPEC || ch->child_count < 1 || !ch->children[0]) continue;
                tok_copy(ch->children[0], src, tbuf, sizeof(tbuf)); ret = tbuf; break;
            }
            json_str(fp, ret, (int)strlen(ret));
            fputs("\"}", fp);
        } else if (top->kind == NODE_TYPE_DECL) {
            if (top->child_count < 1 || !top->children[0]) continue;
            tok_copy(top->children[0], src, nbuf, sizeof(nbuf));
            if (!first) fputs(",", fp); first = 0;
            fputs("\n    {\"kind\": \"type\", \"name\": \"", fp);
            json_str(fp, nbuf, (int)strlen(nbuf));
            fputs("\", \"fields\": [", fp);
            int ff = 1;
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_FIELD || ch->child_count < 2 || !ch->children[0] || !ch->children[1]) continue;
                char fn[128], ft[128];
                tok_copy(ch->children[0], src, fn, sizeof(fn));
                tok_copy(ch->children[1], src, ft, sizeof(ft));
                if (!ff) fputs(", ", fp); ff = 0;
                fputs("{\"name\": \"", fp); json_str(fp, fn, (int)strlen(fn));
                fputs("\", \"type\": \"", fp); json_str(fp, ft, (int)strlen(ft));
                fputs("\"}", fp);
            }
            fputs("]}", fp);
        } else if (top->kind == NODE_CONST_DECL) {
            if (top->child_count < 1 || !top->children[0]) continue;
            tok_copy(top->children[0], src, nbuf, sizeof(nbuf));
            if (!first) fputs(",", fp); first = 0;
            fputs("\n    {\"kind\": \"const\", \"name\": \"", fp);
            json_str(fp, nbuf, (int)strlen(nbuf));
            fputs("\", \"type\": \"", fp);
            const char *ty = "unknown";
            if (top->child_count >= 2 && top->children[1] &&
                    top->children[1]->kind == NODE_TYPE_EXPR) {
                tok_copy(top->children[1], src, tbuf, sizeof(tbuf)); ty = tbuf;
            }
            json_str(fp, ty, (int)strlen(ty));
            fputs("\"}", fp);
        }
    }
    if (!first) fputs("\n  ", fp);
    fputs("]\n}\n", fp);
    if (fclose(fp) != 0) {
        diag_emit(DIAG_ERROR, E9001, 0, 0, 0,
                  "failed to write interface file '%s': %s",
                  out_path, strerror(errno), (char *)NULL);
        return -1;
    }
    return 0;
}

uint32_t interface_hash(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint32_t h = 5381u;
    int c;
    while ((c = fgetc(fp)) != EOF)
        h = ((h << 5) + h) ^ (uint32_t)c;
    fclose(fp);
    return h;
}
