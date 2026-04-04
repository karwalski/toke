/*
 * companion.c — Companion file (.tkc.md) skeleton generator for the toke
 *               reference compiler.
 *
 * =========================================================================
 * Role
 * =========================================================================
 * Walks a validated AST and emits a deterministic markdown skeleton that
 * follows the companion file format spec (docs/companion-file-spec.md).
 *
 * The skeleton includes:
 *   - YAML frontmatter (source_file, source_hash, compiler_version, etc.)
 *   - ## Module section with the module name
 *   - ## Types section with field tables
 *   - ## Functions section with signature, parameter table, return info
 *   - ## Constants section with name and type
 *   - ## Control Flow placeholder
 *
 * Prose fields are emitted as <!-- TODO: describe ... --> placeholders.
 * The compiler generates structure, not explanations.
 *
 * Story: 11.5.2, 11.5.4
 */

#include "companion.h"
#include "tkc_limits.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Minimal SHA-256 (portable C99) ──────────────────────────────────── */

/*
 * Self-contained SHA-256 implementation.  Only used by emit_companion()
 * to hash the source file content for the source_hash frontmatter field.
 * Not intended for general use — the stdlib crypto module is the public API.
 */

static const unsigned int K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define RR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (RR(x,2)^RR(x,13)^RR(x,22))
#define EP1(x) (RR(x,6)^RR(x,11)^RR(x,25))
#define SIG0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define SIG1(x) (RR(x,17)^RR(x,19)^((x)>>10))

typedef struct {
    unsigned int state[8];
    unsigned char buf[64];
    unsigned long long total;
} Sha256;

static void sha256_transform(Sha256 *ctx)
{
    unsigned int w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)ctx->buf[i*4]<<24) |
               ((unsigned int)ctx->buf[i*4+1]<<16) |
               ((unsigned int)ctx->buf[i*4+2]<<8) |
               ((unsigned int)ctx->buf[i*4+3]);
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (int i = 0; i < 64; i++) {
        unsigned int t1 = h + EP1(e) + CH(e,f,g) + K256[i] + w[i];
        unsigned int t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(Sha256 *ctx)
{
    ctx->state[0]=0x6a09e667u; ctx->state[1]=0xbb67ae85u;
    ctx->state[2]=0x3c6ef372u; ctx->state[3]=0xa54ff53au;
    ctx->state[4]=0x510e527fu; ctx->state[5]=0x9b05688cu;
    ctx->state[6]=0x1f83d9abu; ctx->state[7]=0x5be0cd19u;
    ctx->total = 0;
}

static void sha256_update(Sha256 *ctx, const unsigned char *data, unsigned long len)
{
    unsigned long i = 0;
    unsigned int idx = (unsigned int)(ctx->total & 63);
    ctx->total += len;
    for (; i < len; i++) {
        ctx->buf[idx++] = data[i];
        if (idx == 64) { sha256_transform(ctx); idx = 0; }
    }
}

static void sha256_final(Sha256 *ctx, unsigned char hash[32])
{
    unsigned int idx = (unsigned int)(ctx->total & 63);
    ctx->buf[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buf[idx++] = 0;
        sha256_transform(ctx);
        idx = 0;
    }
    while (idx < 56) ctx->buf[idx++] = 0;
    unsigned long long bits = ctx->total * 8;
    for (int i = 7; i >= 0; i--)
        ctx->buf[56 + (7-i)] = (unsigned char)(bits >> (i*8));
    sha256_transform(ctx);
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (unsigned char)(ctx->state[i]>>24);
        hash[i*4+1] = (unsigned char)(ctx->state[i]>>16);
        hash[i*4+2] = (unsigned char)(ctx->state[i]>>8);
        hash[i*4+3] = (unsigned char)(ctx->state[i]);
    }
}

/* ── AST helpers ─────────────────────────────────────────────────────── */

/*
 * tok_text — Copy the source token text of an AST node into a buffer.
 *
 * Returns buf for convenient inline use.
 */
static const char *tok_text(const Node *n, const char *src,
                            char *buf, int size)
{
    if (!n) { buf[0] = '\0'; return buf; }
    int len = n->tok_len < size - 1 ? n->tok_len : size - 1;
    memcpy(buf, src + n->tok_start, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/*
 * get_module_name — Extract the module name string from the AST.
 *
 * Scans top-level children for NODE_MODULE -> NODE_MODULE_PATH and
 * concatenates identifier segments with '.' separators.
 */
static void get_module_name(const Node *ast, const char *src,
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
                int len = id->tok_len < size - 1 - pos
                        ? id->tok_len : size - 1 - pos;
                memcpy(buf + pos, src + id->tok_start, (size_t)len);
                pos += len;
            }
            buf[pos] = '\0';
            return;
        }
    }
}

/*
 * get_type_text — Extract a type expression string from a NODE_TYPE_EXPR,
 * NODE_ARRAY_TYPE, NODE_MAP_TYPE, NODE_PTR_TYPE, or leaf type node.
 *
 * For compound types we recurse; for leaf types we just copy the token text.
 */
static void get_type_text(const Node *n, const char *src,
                          char *buf, int size)
{
    if (!n) { snprintf(buf, (size_t)size, "unknown"); return; }

    if (n->kind == NODE_ARRAY_TYPE) {
        char elem[NAME_BUF];
        if (n->child_count > 0 && n->children[0])
            get_type_text(n->children[0], src, elem, (int)sizeof(elem));
        else
            snprintf(elem, sizeof(elem), "unknown");
        snprintf(buf, (size_t)size, "@(%s)", elem);
        return;
    }
    if (n->kind == NODE_MAP_TYPE) {
        char key[NAME_BUF], val[NAME_BUF];
        if (n->child_count > 0 && n->children[0])
            get_type_text(n->children[0], src, key, (int)sizeof(key));
        else
            snprintf(key, sizeof(key), "unknown");
        if (n->child_count > 1 && n->children[1])
            get_type_text(n->children[1], src, val, (int)sizeof(val));
        else
            snprintf(val, sizeof(val), "unknown");
        snprintf(buf, (size_t)size, "@(%s:%s)", key, val);
        return;
    }
    if (n->kind == NODE_PTR_TYPE) {
        char inner[NAME_BUF];
        if (n->child_count > 0 && n->children[0])
            get_type_text(n->children[0], src, inner, (int)sizeof(inner));
        else
            snprintf(inner, sizeof(inner), "unknown");
        snprintf(buf, (size_t)size, "*%s", inner);
        return;
    }

    /* Leaf: just copy token text */
    tok_text(n, src, buf, size);
}

/* ── Section emitters ────────────────────────────────────────────────── */

/*
 * emit_frontmatter — Write the YAML frontmatter block.
 */
static void emit_frontmatter(FILE *out, const char *source_path,
                              const char *source, long source_len)
{
    /* Extract just the filename from the path */
    const char *basename = strrchr(source_path, '/');
    basename = basename ? basename + 1 : source_path;

    /* Compute SHA-256 */
    Sha256 ctx;
    unsigned char hash[32];
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)source, (unsigned long)source_len);
    sha256_final(&ctx, hash);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);

    /* ISO 8601 timestamp */
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", utc);

    fprintf(out, "---\n");
    fprintf(out, "source_file: \"%s\"\n", basename);
    fprintf(out, "source_hash: \"%s\"\n", hex);
    fprintf(out, "compiler_version: \"0.1.0\"\n");
    fprintf(out, "generated_at: \"%s\"\n", ts);
    fprintf(out, "format_version: \"1.0\"\n");
    fprintf(out, "---\n");
}

/*
 * emit_module_section — Write the ## Module section.
 */
static void emit_module_section(FILE *out, const Node *ast, const char *src)
{
    char mod_name[NAME_BUF];
    get_module_name(ast, src, mod_name, (int)sizeof(mod_name));

    fprintf(out, "\n## Module\n\n");
    fprintf(out, "**Name:** `%s`\n\n", mod_name[0] ? mod_name : "unnamed");
    fprintf(out, "<!-- TODO: describe what this module does -->\n\n");

    /* Check for imports */
    int has_imports = 0;
    for (int i = 0; i < ast->child_count; i++) {
        if (ast->children[i] && ast->children[i]->kind == NODE_IMPORT) {
            has_imports = 1;
            break;
        }
    }

    if (has_imports) {
        fprintf(out, "**Imports:**\n\n");
        for (int i = 0; i < ast->child_count; i++) {
            const Node *n = ast->children[i];
            if (!n || n->kind != NODE_IMPORT) continue;
            /* Import node: child[0] is alias, child[1] is module path */
            if (n->child_count >= 2 && n->children[0] && n->children[1]) {
                char alias[NAME_BUF];
                tok_text(n->children[0], src, alias, (int)sizeof(alias));
                /* Build module path */
                const Node *mp = n->children[1];
                if (mp->kind == NODE_MODULE_PATH) {
                    char path[NAME_BUF];
                    int pos = 0;
                    for (int k = 0; k < mp->child_count && pos < (int)sizeof(path) - 1; k++) {
                        const Node *seg = mp->children[k];
                        if (!seg) continue;
                        if (pos > 0 && pos < (int)sizeof(path) - 2)
                            path[pos++] = '.';
                        int slen = seg->tok_len < (int)sizeof(path) - 1 - pos
                                 ? seg->tok_len : (int)sizeof(path) - 1 - pos;
                        memcpy(path + pos, src + seg->tok_start, (size_t)slen);
                        pos += slen;
                    }
                    path[pos] = '\0';
                    fprintf(out, "- `%s` (`%s`) — <!-- TODO: describe import -->\n",
                            alias, path);
                } else {
                    char mpath[NAME_BUF];
                    tok_text(mp, src, mpath, (int)sizeof(mpath));
                    fprintf(out, "- `%s` (`%s`) — <!-- TODO: describe import -->\n",
                            alias, mpath);
                }
            }
        }
        fprintf(out, "\n");
    } else {
        fprintf(out, "**Imports:** None.\n");
    }
}

/*
 * emit_types_section — Write the ## Types section.
 */
static void emit_types_section(FILE *out, const Node *ast, const char *src)
{
    /* Check if there are any type declarations */
    int has_types = 0;
    for (int i = 0; i < ast->child_count; i++) {
        if (ast->children[i] && ast->children[i]->kind == NODE_TYPE_DECL) {
            has_types = 1;
            break;
        }
    }
    if (!has_types) return;

    fprintf(out, "\n## Types\n");

    for (int i = 0; i < ast->child_count; i++) {
        const Node *top = ast->children[i];
        if (!top || top->kind != NODE_TYPE_DECL) continue;
        if (top->child_count < 1 || !top->children[0]) continue;

        char name[NAME_BUF];
        tok_text(top->children[0], src, name, (int)sizeof(name));

        fprintf(out, "\n### `$%s`\n\n", name);
        fprintf(out, "<!-- TODO: describe what this type represents -->\n\n");

        /* Collect fields */
        int has_fields = 0;
        for (int j = 1; j < top->child_count; j++) {
            const Node *ch = top->children[j];
            if (ch && ch->kind == NODE_FIELD) { has_fields = 1; break; }
        }

        if (has_fields) {
            fprintf(out, "| Field | Type | Description |\n");
            fprintf(out, "|-------|------|-------------|\n");
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_FIELD) continue;
                if (ch->child_count < 2 || !ch->children[0] || !ch->children[1])
                    continue;
                char fname[NAME_BUF], ftype[NAME_BUF];
                tok_text(ch->children[0], src, fname, (int)sizeof(fname));
                get_type_text(ch->children[1], src, ftype, (int)sizeof(ftype));
                fprintf(out, "| `%s` | `%s` | <!-- TODO: describe field --> |\n",
                        fname, ftype);
            }
        }
    }
}

/*
 * emit_functions_section — Write the ## Functions section.
 */
static void emit_functions_section(FILE *out, const Node *ast, const char *src)
{
    /* Check if there are any function declarations */
    int has_funcs = 0;
    for (int i = 0; i < ast->child_count; i++) {
        if (ast->children[i] && ast->children[i]->kind == NODE_FUNC_DECL) {
            has_funcs = 1;
            break;
        }
    }
    if (!has_funcs) return;

    fprintf(out, "\n## Functions\n");

    for (int i = 0; i < ast->child_count; i++) {
        const Node *top = ast->children[i];
        if (!top || top->kind != NODE_FUNC_DECL) continue;
        if (top->child_count < 1 || !top->children[0]) continue;

        char fname[NAME_BUF];
        tok_text(top->children[0], src, fname, (int)sizeof(fname));

        /* Build the signature string: name(param1: type1, ...): rettype */
        /* Collect params */
        char sig[CMD_BUF];
        int spos = snprintf(sig, sizeof(sig), "%s(", fname);

        int param_idx = 0;
        for (int j = 1; j < top->child_count; j++) {
            const Node *ch = top->children[j];
            if (!ch || ch->kind != NODE_PARAM) continue;
            if (ch->child_count < 2 || !ch->children[0] || !ch->children[1])
                continue;
            char pname[NAME_BUF], ptype[NAME_BUF];
            tok_text(ch->children[0], src, pname, (int)sizeof(pname));
            get_type_text(ch->children[1], src, ptype, (int)sizeof(ptype));
            if (param_idx > 0)
                spos += snprintf(sig + spos, sizeof(sig) - (size_t)spos, ", ");
            spos += snprintf(sig + spos, sizeof(sig) - (size_t)spos,
                             "%s: %s", pname, ptype);
            param_idx++;
        }
        spos += snprintf(sig + spos, sizeof(sig) - (size_t)spos, ")");

        /* Return type */
        const char *ret = "void";
        char ret_buf[NAME_BUF];
        for (int j = 1; j < top->child_count; j++) {
            const Node *ch = top->children[j];
            if (!ch || ch->kind != NODE_RETURN_SPEC) continue;
            if (ch->child_count < 1 || !ch->children[0]) continue;
            get_type_text(ch->children[0], src, ret_buf, (int)sizeof(ret_buf));
            ret = ret_buf;
            break;
        }
        snprintf(sig + spos, sizeof(sig) - (size_t)spos, ": %s", ret);

        fprintf(out, "\n### `%s`\n\n", sig);
        fprintf(out, "**Purpose:** <!-- TODO: describe purpose -->\n\n");

        /* Parameter table */
        if (param_idx > 0) {
            fprintf(out, "**Parameters:**\n\n");
            fprintf(out, "| Name | Type | Description |\n");
            fprintf(out, "|------|------|-------------|\n");
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_PARAM) continue;
                if (ch->child_count < 2 || !ch->children[0] || !ch->children[1])
                    continue;
                char pname[NAME_BUF], ptype[NAME_BUF];
                tok_text(ch->children[0], src, pname, (int)sizeof(pname));
                get_type_text(ch->children[1], src, ptype, (int)sizeof(ptype));
                fprintf(out, "| `%s` | `%s` | <!-- TODO: describe parameter --> |\n",
                        pname, ptype);
            }
            fprintf(out, "\n");
        } else {
            fprintf(out, "**Parameters:** None.\n\n");
        }

        fprintf(out, "**Returns:** `%s`", ret);
        if (strcmp(ret, "void") != 0)
            fprintf(out, " — <!-- TODO: describe return value -->");
        fprintf(out, "\n\n");

        fprintf(out, "**Logic:**\n\n");
        fprintf(out, "<!-- TODO: step-by-step description of the implementation -->\n\n");

        fprintf(out, "**Error handling:** <!-- TODO: describe error handling, or \"None\" -->\n");
    }
}

/*
 * emit_constants_section — Write the ## Constants section.
 */
static void emit_constants_section(FILE *out, const Node *ast, const char *src)
{
    /* Check if there are any top-level constant declarations */
    int has_consts = 0;
    for (int i = 0; i < ast->child_count; i++) {
        if (ast->children[i] && ast->children[i]->kind == NODE_CONST_DECL) {
            has_consts = 1;
            break;
        }
    }
    if (!has_consts) return;

    fprintf(out, "\n## Constants\n\n");
    fprintf(out, "| Name | Type | Description |\n");
    fprintf(out, "|------|------|-------------|\n");

    for (int i = 0; i < ast->child_count; i++) {
        const Node *top = ast->children[i];
        if (!top || top->kind != NODE_CONST_DECL) continue;
        if (top->child_count < 1 || !top->children[0]) continue;

        char cname[NAME_BUF];
        tok_text(top->children[0], src, cname, (int)sizeof(cname));

        const char *ctype = "unknown";
        char ctype_buf[NAME_BUF];
        if (top->child_count >= 2 && top->children[1]) {
            get_type_text(top->children[1], src, ctype_buf, (int)sizeof(ctype_buf));
            ctype = ctype_buf;
        }

        fprintf(out, "| `%s` | `%s` | <!-- TODO: describe constant --> |\n",
                cname, ctype);
    }
}

/*
 * emit_control_flow_section — Write the ## Control Flow placeholder.
 */
static void emit_control_flow_section(FILE *out)
{
    fprintf(out, "\n## Control Flow\n\n");
    fprintf(out, "<!-- TODO: describe overall execution flow, entry point, ");
    fprintf(out, "key branching/looping patterns, data flow between functions -->\n");
}

/* ── Public API ──────────────────────────────────────────────────────── */

void emit_companion(FILE *out, const char *source_path, const char *source,
                    long source_len, const Node *ast)
{
    emit_frontmatter(out, source_path, source, source_len);
    emit_module_section(out, ast, source);
    emit_types_section(out, ast, source);
    emit_functions_section(out, ast, source);
    emit_constants_section(out, ast, source);
    emit_control_flow_section(out);
}

/* ── Companion file verification ─────────────────────────────────────── */

/*
 * extract_frontmatter_field — Scan YAML frontmatter for a field and copy
 * its quoted value into buf.  Returns 1 on success, 0 if not found.
 *
 * Expects lines of the form:  key: "value"
 * The frontmatter region is delimited by the first two "---" lines.
 */
static int extract_frontmatter_field(const char *text, const char *key,
                                     char *buf, int buf_size)
{
    buf[0] = '\0';
    /* Find opening --- */
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p[0] != '-' || p[1] != '-' || p[2] != '-') return 0;
    p += 3;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;

    int key_len = (int)strlen(key);

    /* Scan lines until closing --- */
    while (*p) {
        /* Check for closing --- */
        const char *line = p;
        if (line[0] == '-' && line[1] == '-' && line[2] == '-') break;

        /* Check if this line starts with the key */
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, (size_t)key_len) && p[key_len] == ':') {
            p += key_len + 1;
            /* Skip spaces and optional quote */
            while (*p == ' ' || *p == '\t') p++;
            char quote = 0;
            if (*p == '"' || *p == '\'') { quote = *p; p++; }
            int i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < buf_size - 1) {
                if (quote && *p == quote) break;
                buf[i++] = *p++;
            }
            buf[i] = '\0';
            return 1;
        }

        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

int verify_companion(const char *companion_path)
{
    /* Read companion file */
    FILE *cf = fopen(companion_path, "rb");
    if (!cf) {
        fprintf(stderr, "tkc: cannot open companion file '%s'\n",
                companion_path);
        return 2;
    }
    fseek(cf, 0, SEEK_END);
    long clen = ftell(cf);
    rewind(cf);
    char *cbuf = malloc((size_t)clen + 1);
    if (!cbuf) {
        fclose(cf);
        fprintf(stderr, "tkc: out of memory reading '%s'\n", companion_path);
        return 2;
    }
    if ((long)fread(cbuf, 1, (size_t)clen, cf) != clen) {
        fclose(cf); free(cbuf);
        fprintf(stderr, "tkc: failed to read '%s'\n", companion_path);
        return 2;
    }
    cbuf[clen] = '\0';
    fclose(cf);

    /* Extract source_file and source_hash from frontmatter */
    char source_file[PATH_BUF];
    char expected_hash[128];
    if (!extract_frontmatter_field(cbuf, "source_file", source_file,
                                   (int)sizeof(source_file))) {
        free(cbuf);
        fprintf(stderr,
                "tkc: companion file missing 'source_file' field\n");
        return 2;
    }
    if (!extract_frontmatter_field(cbuf, "source_hash", expected_hash,
                                   (int)sizeof(expected_hash))) {
        free(cbuf);
        fprintf(stderr,
                "tkc: companion file missing 'source_hash' field\n");
        return 2;
    }
    free(cbuf);

    /* Resolve source_file relative to the companion file's directory */
    char source_path[PATH_BUF];
    const char *last_slash = strrchr(companion_path, '/');
    if (last_slash) {
        int dir_len = (int)(last_slash - companion_path + 1);
        if (dir_len + (int)strlen(source_file) >= (int)sizeof(source_path)) {
            fprintf(stderr, "tkc: source path too long\n");
            return 2;
        }
        memcpy(source_path, companion_path, (size_t)dir_len);
        source_path[dir_len] = '\0';
        strncat(source_path, source_file,
                sizeof(source_path) - (size_t)dir_len - 1);
    } else {
        strncpy(source_path, source_file, sizeof(source_path) - 1);
        source_path[sizeof(source_path) - 1] = '\0';
    }

    /* Read source file */
    FILE *sf = fopen(source_path, "rb");
    if (!sf) {
        fprintf(stderr, "tkc: cannot open source file '%s'\n", source_path);
        return 2;
    }
    fseek(sf, 0, SEEK_END);
    long slen = ftell(sf);
    rewind(sf);
    char *sbuf = malloc((size_t)slen + 1);
    if (!sbuf) {
        fclose(sf);
        fprintf(stderr, "tkc: out of memory reading '%s'\n", source_path);
        return 2;
    }
    if ((long)fread(sbuf, 1, (size_t)slen, sf) != slen) {
        fclose(sf); free(sbuf);
        fprintf(stderr, "tkc: failed to read '%s'\n", source_path);
        return 2;
    }
    fclose(sf);

    /* Compute SHA-256 of the source file */
    Sha256 ctx;
    unsigned char hash[32];
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)sbuf, (unsigned long)slen);
    sha256_final(&ctx, hash);
    free(sbuf);

    char actual_hash[65];
    for (int i = 0; i < 32; i++)
        snprintf(actual_hash + i * 2, 3, "%02x", hash[i]);

    /* Compare */
    if (!strcmp(expected_hash, actual_hash)) {
        printf("MATCH: %s (hash: %.8s...)\n", source_file, actual_hash);
        return 0;
    } else {
        printf("MISMATCH: %s expected %s got %s\n",
               source_file, expected_hash, actual_hash);
        return 1;
    }
}

/* ── Companion diff ──────────────────────────────────────────────────── */

/*
 * DiffEntry — a function or type extracted from either the fresh skeleton
 * or the existing companion file for structural comparison.
 */
#define DIFF_MAX_ENTRIES 256
#define DIFF_KIND_FUNC   0
#define DIFF_KIND_TYPE   1

typedef struct {
    char name[NAME_BUF];   /* function or type name                     */
    char sig[CMD_BUF];     /* full signature line (for functions)       */
    int  kind;             /* DIFF_KIND_FUNC or DIFF_KIND_TYPE          */
} DiffEntry;

/*
 * extract_entries_from_md — Scan a companion markdown buffer for ### headings
 * that define functions or types.
 *
 * Function headings look like:  ### `fname(param: type, ...): rettype`
 * Type headings look like:      ### `$TypeName`
 *
 * Populates entries[] up to max_entries.  Returns number of entries found.
 */
static int extract_entries_from_md(const char *md, DiffEntry *entries,
                                   int max_entries)
{
    int count = 0;
    const char *p = md;

    while (*p && count < max_entries) {
        /* Look for ### ` at start of a line */
        if (p[0] == '#' && p[1] == '#' && p[2] == '#' && p[3] == ' ' &&
            p[4] == '`') {
            const char *start = p + 5;
            /* Find closing backtick */
            const char *end = start;
            while (*end && *end != '`' && *end != '\n') end++;
            if (*end == '`') {
                int len = (int)(end - start);
                DiffEntry *e = &entries[count];

                /* Copy full signature */
                int slen = len < (int)sizeof(e->sig) - 1
                         ? len : (int)sizeof(e->sig) - 1;
                memcpy(e->sig, start, (size_t)slen);
                e->sig[slen] = '\0';

                /* Determine kind and extract name */
                if (start[0] == '$') {
                    /* Type: $TypeName */
                    e->kind = DIFF_KIND_TYPE;
                    int nlen = slen < (int)sizeof(e->name) - 1
                             ? slen : (int)sizeof(e->name) - 1;
                    memcpy(e->name, start, (size_t)nlen);
                    e->name[nlen] = '\0';
                } else {
                    /* Function: name(... */
                    e->kind = DIFF_KIND_FUNC;
                    const char *paren = start;
                    while (paren < end && *paren != '(') paren++;
                    int nlen = (int)(paren - start);
                    if (nlen >= (int)sizeof(e->name))
                        nlen = (int)sizeof(e->name) - 1;
                    memcpy(e->name, start, (size_t)nlen);
                    e->name[nlen] = '\0';
                }
                count++;
            }
        }

        /* Advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return count;
}

/*
 * extract_entries_from_ast — Walk the AST and build DiffEntry records for
 * all top-level functions and types, using the same signature format that
 * emit_companion generates.
 */
static int extract_entries_from_ast(const Node *ast, const char *src,
                                    DiffEntry *entries, int max_entries)
{
    int count = 0;

    for (int i = 0; i < ast->child_count && count < max_entries; i++) {
        const Node *top = ast->children[i];
        if (!top) continue;

        if (top->kind == NODE_TYPE_DECL) {
            if (top->child_count < 1 || !top->children[0]) continue;
            DiffEntry *e = &entries[count];
            e->kind = DIFF_KIND_TYPE;
            char tname[NAME_BUF];
            tok_text(top->children[0], src, tname, (int)sizeof(tname));
            snprintf(e->name, sizeof(e->name), "$%s", tname);
            snprintf(e->sig, sizeof(e->sig), "$%s", tname);

            /* Append field info for structural comparison */
            int spos = (int)strlen(e->sig);
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_FIELD) continue;
                if (ch->child_count < 2 || !ch->children[0] || !ch->children[1])
                    continue;
                char fname[NAME_BUF], ftype[NAME_BUF];
                tok_text(ch->children[0], src, fname, (int)sizeof(fname));
                get_type_text(ch->children[1], src, ftype, (int)sizeof(ftype));
                spos += snprintf(e->sig + spos,
                                 sizeof(e->sig) - (size_t)spos,
                                 " %s:%s", fname, ftype);
            }
            count++;
        }

        if (top->kind == NODE_FUNC_DECL) {
            if (top->child_count < 1 || !top->children[0]) continue;
            DiffEntry *e = &entries[count];
            e->kind = DIFF_KIND_FUNC;

            char fname[NAME_BUF];
            tok_text(top->children[0], src, fname, (int)sizeof(fname));
            strncpy(e->name, fname, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';

            /* Build signature: name(param: type, ...): rettype */
            int spos = snprintf(e->sig, sizeof(e->sig), "%s(", fname);
            int param_idx = 0;
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_PARAM) continue;
                if (ch->child_count < 2 || !ch->children[0] || !ch->children[1])
                    continue;
                char pname[NAME_BUF], ptype[NAME_BUF];
                tok_text(ch->children[0], src, pname, (int)sizeof(pname));
                get_type_text(ch->children[1], src, ptype, (int)sizeof(ptype));
                if (param_idx > 0)
                    spos += snprintf(e->sig + spos,
                                     sizeof(e->sig) - (size_t)spos, ", ");
                spos += snprintf(e->sig + spos,
                                 sizeof(e->sig) - (size_t)spos,
                                 "%s: %s", pname, ptype);
                param_idx++;
            }
            spos += snprintf(e->sig + spos,
                             sizeof(e->sig) - (size_t)spos, ")");

            /* Return type */
            const char *ret = "void";
            char ret_buf[NAME_BUF];
            for (int j = 1; j < top->child_count; j++) {
                const Node *ch = top->children[j];
                if (!ch || ch->kind != NODE_RETURN_SPEC) continue;
                if (ch->child_count < 1 || !ch->children[0]) continue;
                get_type_text(ch->children[0], src, ret_buf,
                              (int)sizeof(ret_buf));
                ret = ret_buf;
                break;
            }
            snprintf(e->sig + spos, sizeof(e->sig) - (size_t)spos,
                     ": %s", ret);
            count++;
        }
    }
    return count;
}

/*
 * find_entry — Search entries for a matching name and kind.
 * Returns index or -1 if not found.
 */
static int find_entry(const DiffEntry *entries, int count,
                      const char *name, int kind)
{
    for (int i = 0; i < count; i++) {
        if (entries[i].kind == kind && !strcmp(entries[i].name, name))
            return i;
    }
    return -1;
}

int companion_diff(const char *source_path, const char *source,
                   long source_len, const Node *ast,
                   const char *companion_path)
{
    (void)source_path;  /* reserved for future use (e.g. path display) */

    /* Read existing companion file */
    FILE *cf = fopen(companion_path, "rb");
    if (!cf) {
        fprintf(stderr, "tkc: cannot open companion file '%s'\n",
                companion_path);
        return 2;
    }
    fseek(cf, 0, SEEK_END);
    long clen = ftell(cf);
    rewind(cf);
    char *cbuf = malloc((size_t)clen + 1);
    if (!cbuf) {
        fclose(cf);
        fprintf(stderr, "tkc: out of memory reading '%s'\n",
                companion_path);
        return 2;
    }
    if ((long)fread(cbuf, 1, (size_t)clen, cf) != clen) {
        fclose(cf); free(cbuf);
        fprintf(stderr, "tkc: failed to read '%s'\n", companion_path);
        return 2;
    }
    cbuf[clen] = '\0';
    fclose(cf);

    int divergences = 0;

    /* Check source_hash */
    char expected_hash[128];
    if (extract_frontmatter_field(cbuf, "source_hash", expected_hash,
                                   (int)sizeof(expected_hash))) {
        Sha256 ctx;
        unsigned char hash[32];
        sha256_init(&ctx);
        sha256_update(&ctx, (const unsigned char *)source,
                      (unsigned long)source_len);
        sha256_final(&ctx, hash);
        char actual_hash[65];
        for (int i = 0; i < 32; i++)
            snprintf(actual_hash + i * 2, 3, "%02x", hash[i]);
        if (strcmp(expected_hash, actual_hash)) {
            printf("HASH     source changed since companion was written\n");
            divergences++;
        }
    }

    /* Extract entries from existing companion */
    DiffEntry *comp_entries = malloc(DIFF_MAX_ENTRIES * sizeof(DiffEntry));
    DiffEntry *ast_entries  = malloc(DIFF_MAX_ENTRIES * sizeof(DiffEntry));
    if (!comp_entries || !ast_entries) {
        free(cbuf); free(comp_entries); free(ast_entries);
        fprintf(stderr, "tkc: out of memory in companion_diff\n");
        return 2;
    }

    int comp_count = extract_entries_from_md(cbuf, comp_entries,
                                             DIFF_MAX_ENTRIES);
    free(cbuf);

    /* Extract entries from AST */
    int ast_count = extract_entries_from_ast(ast, source, ast_entries,
                                            DIFF_MAX_ENTRIES);

    /* Report NEW: in AST but not in companion */
    for (int i = 0; i < ast_count; i++) {
        int j = find_entry(comp_entries, comp_count,
                           ast_entries[i].name, ast_entries[i].kind);
        if (j < 0) {
            const char *label = ast_entries[i].kind == DIFF_KIND_FUNC
                              ? "function" : "type";
            printf("NEW      %s `%s`\n", label, ast_entries[i].name);
            divergences++;
        }
    }

    /* Report REMOVED: in companion but not in AST */
    for (int i = 0; i < comp_count; i++) {
        int j = find_entry(ast_entries, ast_count,
                           comp_entries[i].name, comp_entries[i].kind);
        if (j < 0) {
            const char *label = comp_entries[i].kind == DIFF_KIND_FUNC
                              ? "function" : "type";
            printf("REMOVED  %s `%s`\n", label, comp_entries[i].name);
            divergences++;
        }
    }

    /* Report CHANGED: present in both but signatures differ */
    for (int i = 0; i < ast_count; i++) {
        int j = find_entry(comp_entries, comp_count,
                           ast_entries[i].name, ast_entries[i].kind);
        if (j >= 0 && strcmp(ast_entries[i].sig, comp_entries[j].sig)) {
            const char *label = ast_entries[i].kind == DIFF_KIND_FUNC
                              ? "function" : "type";
            printf("CHANGED  %s `%s`\n", label, ast_entries[i].name);
            printf("  source:    %s\n", ast_entries[i].sig);
            printf("  companion: %s\n", comp_entries[j].sig);
            divergences++;
        }
    }

    free(comp_entries);
    free(ast_entries);

    if (divergences == 0) {
        printf("OK: companion file is in sync with source\n");
    } else {
        printf("\n%d divergence%s found\n", divergences,
               divergences == 1 ? "" : "s");
    }

    return divergences > 0 ? 1 : 0;
}

/*
 * companion_diff_from_skeleton — Alternative: compare the raw generated
 * skeleton text against the existing companion.  Not currently used,
 * but available for future line-by-line diff features.
 */
