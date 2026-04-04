/*
 * ir.c — Interface file emitter for the toke reference compiler.
 *
 * =========================================================================
 * Role in the compiler pipeline
 * =========================================================================
 * The interface emitter runs after type checking when the --emit-interface
 * flag is set.  It walks the top-level AST nodes and serialises exported
 * declarations into a JSON .tki (toke interface) file.
 *
 * Interface files let downstream modules and tooling inspect a module's
 * public surface without re-parsing source.  The schema_version field
 * allows future format evolution.
 *
 * =========================================================================
 * Output format (.tki)
 * =========================================================================
 * The emitter writes a JSON object with:
 *   schema_version  "1.0"
 *   module          dotted module path (e.g. "std.math")
 *   version         always "1.0.0" for now
 *   exports[]       array of export descriptors:
 *     kind "func"   — name, params (type strings), return type, extern flag
 *     kind "type"   — name, fields (name + type)
 *     kind "const"  — name, type
 *
 * =========================================================================
 * Hashing
 * =========================================================================
 * interface_hash() provides a lightweight djb2 hash of a .tki file.
 * Callers can compare hashes across builds to detect interface changes
 * (useful for incremental compilation gating).
 *
 * Story: 1.2.7
 */

#include "ir.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

/*
 * type_to_str — Convert a TypeKind enum into its toke source representation.
 *
 * Writes the string form of the type (e.g. "i64", "bool", "[str]") into the
 * caller-supplied buffer.  Handles primitives directly and recurses for
 * compound types (arrays, error types, function types).  Falls back to
 * "unknown" for NULL or unrecognised type kinds.
 *
 * Currently unused by emit_interface() (which extracts type names from
 * source tokens instead), but retained for future use by richer interface
 * formats that need resolved types.
 */
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

/*
 * json_str — Write a JSON-escaped string to a file stream.
 *
 * Emits each character of s[0..len-1] to fp, escaping characters that
 * are special in JSON: double-quote, backslash, newline, carriage return,
 * tab, and any control character below U+0020.  Does NOT write surrounding
 * quotes; the caller is responsible for those.
 */
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

/*
 * collect_module_path — Extract the dotted module path from the AST.
 *
 * Scans the top-level children of ast for a NODE_MODULE node, then its
 * child NODE_MODULE_PATH.  Each identifier child of the path is
 * concatenated with '.' separators into buf (e.g. "std.math").
 *
 * If no module declaration exists the buffer is left empty.  The function
 * returns on the first MODULE_PATH found.
 */
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

/*
 * tok_copy — Copy the source token text of an AST node into a buffer.
 *
 * Extracts at most size-1 bytes of the node's token span from the source
 * string, NUL-terminates the result, and returns buf.  Used throughout
 * emit_interface() to lift identifier and type names out of the source.
 */
static const char *tok_copy(const Node *n, const char *src, char *buf, int size)
{
    int len = n->tok_len < size - 1 ? n->tok_len : size - 1;
    memcpy(buf, src + n->tok_start, len);
    buf[len] = '\0';
    return buf;
}

/*
 * emit_interface — Write a .tki JSON interface file for the given AST.
 *
 * Walks every top-level AST node and emits JSON export descriptors:
 *   NODE_FUNC_DECL  -> {"kind":"func", "name":..., "params":[...], "return":...}
 *                      Also sets "extern":true for bodyless (extern) functions.
 *   NODE_TYPE_DECL  -> {"kind":"type", "name":..., "fields":[{name,type},...]}
 *   NODE_CONST_DECL -> {"kind":"const", "name":..., "type":...}
 *
 * Names and types are extracted directly from source tokens rather than the
 * resolved TypeEnv (type_env is accepted for future richer output but is
 * currently unused).
 *
 * Returns 0 on success, -1 on I/O failure (emits E9001 diagnostic).
 */
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
    fputs("\",\n  \"version\": \"1.0.0\",\n  \"exports\": [", fp);

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
            fputs("\", ", fp);
            /* Check if function is extern (bodyless) */
            { int has_body = 0;
              for (int j = 0; j < top->child_count; j++)
                  if (top->children[j] && top->children[j]->kind == NODE_STMT_LIST) { has_body = 1; break; }
              if (!has_body) fputs("\"extern\": true, ", fp);
            }
            fputs("\"params\": [", fp);
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

/*
 * interface_hash — Compute a djb2 hash of an interface file.
 *
 * Reads the file at path byte-by-byte and feeds each byte through the
 * djb2 XOR variant (h = ((h << 5) + h) ^ c).  Returns 0 if the file
 * cannot be opened.  The hash is not cryptographic; it is intended for
 * fast change detection during incremental builds.
 */
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
