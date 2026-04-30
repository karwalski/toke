/*
 * ast_json.c — AST-to-JSON serialiser for tooling protocol.
 *
 * Walks the parsed AST and emits valid, pretty-printed JSON to a FILE
 * stream.  The output can be piped directly to jq or consumed by any
 * JSON-aware tool (editors, linters, language servers).
 *
 * Story: 10.8.4
 */

#include "ast_json.h"
#include <string.h>

/* ── Node-kind name table ────────────────────────────────────────────── */

static const char *node_kind_name(NodeKind k)
{
    switch (k) {
    case NODE_PROGRAM:        return "PROGRAM";
    case NODE_MODULE:         return "MODULE_DECL";
    case NODE_IMPORT:         return "IMPORT_DECL";
    case NODE_FUNC_DECL:      return "FUNC_DECL";
    case NODE_TYPE_DECL:      return "TYPE_DECL";
    case NODE_CONST_DECL:     return "CONST_DECL";
    case NODE_PARAM:          return "PARAM";
    case NODE_FIELD:          return "FIELD";
    case NODE_STMT_LIST:      return "STMT_LIST";
    case NODE_BIND_STMT:      return "BIND_STMT";
    case NODE_MUT_BIND_STMT:  return "MUT_BIND_STMT";
    case NODE_ASSIGN_STMT:    return "ASSIGN_STMT";
    case NODE_RETURN_STMT:    return "RETURN_STMT";
    case NODE_IF_STMT:        return "IF_STMT";
    case NODE_LOOP_STMT:      return "LOOP_STMT";
    case NODE_BREAK_STMT:     return "BREAK_STMT";
    case NODE_ARENA_STMT:     return "ARENA_STMT";
    case NODE_MATCH_STMT:     return "MATCH_STMT";
    case NODE_EXPR_STMT:      return "EXPR_STMT";
    case NODE_BINARY_EXPR:    return "BINARY_EXPR";
    case NODE_UNARY_EXPR:     return "UNARY_EXPR";
    case NODE_CALL_EXPR:      return "CALL_EXPR";
    case NODE_CAST_EXPR:      return "CAST_EXPR";
    case NODE_PROPAGATE_EXPR: return "PROPAGATE_EXPR";
    case NODE_INDEX_EXPR:     return "INDEX_EXPR";
    case NODE_FIELD_EXPR:     return "FIELD_EXPR";
    case NODE_IDENT:          return "IDENT";
    case NODE_TYPE_IDENT:     return "TYPE_IDENT";
    case NODE_INT_LIT:        return "INT_LIT";
    case NODE_FLOAT_LIT:      return "FLOAT_LIT";
    case NODE_STR_LIT:        return "STR_LIT";
    case NODE_BOOL_LIT:       return "BOOL_LIT";
    case NODE_ARRAY_LIT:      return "ARRAY_LIT";
    case NODE_STRUCT_LIT:     return "STRUCT_LIT";
    case NODE_FIELD_INIT:     return "FIELD_INIT";
    case NODE_TYPE_EXPR:      return "TYPE_EXPR";
    case NODE_ARRAY_TYPE:     return "ARRAY_TYPE";
    case NODE_FUNC_TYPE:      return "FUNC_TYPE";
    case NODE_PTR_TYPE:       return "PTR_TYPE";
    case NODE_MATCH_ARM:      return "MATCH_ARM";
    case NODE_RETURN_SPEC:    return "RETURN_SPEC";
    case NODE_MODULE_PATH:    return "MODULE_PATH";
    case NODE_LOOP_INIT:      return "LOOP_INIT";
    case NODE_MAP_TYPE:       return "MAP_TYPE";
    case NODE_MAP_LIT:        return "MAP_LIT";
    case NODE_MAP_ENTRY:      return "MAP_ENTRY";
    case NODE_FUNC_REF:       return "FUNC_REF";
    case NODE_CLOSURE:        return "CLOSURE";
    case NODE_SCOPE_STMT:     return "SCOPE_STMT";
    case NODE_SPAWN_EXPR:     return "SPAWN_EXPR";
    }
    return "UNKNOWN";
}

/* ── JSON string escaping ────────────────────────────────────────────── */

/*
 * emit_json_string — write a JSON-safe quoted string to out.
 * Escapes backslash, double-quote, and control characters.
 */
static void emit_json_string(FILE *out, const char *s, int len)
{
    fputc('"', out);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out);  break;
        case '\r': fputs("\\r", out);  break;
        case '\t': fputs("\\t", out);  break;
        default:
            if (c < 0x20) {
                fprintf(out, "\\u%04x", c);
            } else {
                fputc((int)c, out);
            }
            break;
        }
    }
    fputc('"', out);
}

/* ── Indentation helper ──────────────────────────────────────────────── */

static void indent(FILE *out, int depth)
{
    for (int i = 0; i < depth; i++) {
        fputs("  ", out);
    }
}

/* ── Recursive node emitter ──────────────────────────────────────────── */

static int is_ident_node(NodeKind k)
{
    return k == NODE_IDENT || k == NODE_TYPE_IDENT || k == NODE_MODULE_PATH;
}

static int is_literal_node(NodeKind k)
{
    return k == NODE_INT_LIT || k == NODE_FLOAT_LIT ||
           k == NODE_STR_LIT || k == NODE_BOOL_LIT;
}

static void emit_node(const Node *n, const char *src, FILE *out, int depth)
{
    if (!n) {
        fputs("null", out);
        return;
    }

    indent(out, depth);
    fputs("{\n", out);

    /* kind */
    indent(out, depth + 1);
    fprintf(out, "\"kind\": \"%s\",\n", node_kind_name(n->kind));

    /* position */
    indent(out, depth + 1);
    fprintf(out, "\"pos\": {\"line\": %d, \"col\": %d, \"offset\": %d},\n",
            n->line, n->col, n->start);

    /* span (tok_start .. tok_start + tok_len) */
    indent(out, depth + 1);
    fprintf(out, "\"span\": {\"start\": %d, \"end\": %d}",
            n->tok_start, n->tok_start + n->tok_len);

    /* name (for identifiers) */
    if (is_ident_node(n->kind) && n->tok_len > 0 && src) {
        fputs(",\n", out);
        indent(out, depth + 1);
        fputs("\"name\": ", out);
        emit_json_string(out, src + n->tok_start, n->tok_len);
    }

    /* value (for literals) */
    if (is_literal_node(n->kind) && n->tok_len > 0 && src) {
        fputs(",\n", out);
        indent(out, depth + 1);
        fputs("\"value\": ", out);
        emit_json_string(out, src + n->tok_start, n->tok_len);
    }

    /* children */
    if (n->child_count > 0) {
        fputs(",\n", out);
        indent(out, depth + 1);
        fputs("\"children\": [\n", out);
        for (int i = 0; i < n->child_count; i++) {
            emit_node(n->children[i], src, out, depth + 2);
            if (i + 1 < n->child_count) {
                fputs(",", out);
            }
            fputs("\n", out);
        }
        indent(out, depth + 1);
        fputc(']', out);
    }

    fputc('\n', out);
    indent(out, depth);
    fputc('}', out);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ast_dump_json(const Node *root, const char *src, FILE *out)
{
    emit_node(root, src, out, 0);
    fputc('\n', out);
}
