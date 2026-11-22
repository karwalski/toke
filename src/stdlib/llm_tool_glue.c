/* SPDX-License-Identifier: MIT */
/*
 * llm_tool_glue.c — i64-ABI wrappers for std.llmtool (story 136.17).
 *
 * The runtime behind this module was complete: llm_tool.c has implemented
 * llm_chatwithtools, llm_submitresult, llm_parse_tool_calls and
 * llm_tool_result_msgs since 17.1.2, and llm_tool.h declares them. Nothing
 * could reach any of it, for three separate reasons at once:
 *
 *   1. the interface named the module `std.llm.tool`, a dotted sub-namespace
 *      that resolves to no .tki and no glue prefix;
 *   2. the documentation headlined it `std.llm_tool`, which the lexer rejects
 *      under 113.2a (E1003, underscore);
 *   3. there were NO _w wrappers at all — this file did not exist.
 *
 * The same class as the secure-memory defect closed in 0fa50e5 (136.5): a
 * working capability published under a name no consumer can write. The module
 * is now `std.llmtool`, which is what a stale twin header llmtool.h had
 * already called it, and the wrappers below are the door.  That twin — and
 * llmtool.c beside it — broke every globbing build with multiple definitions
 * and were removed by 127.99; llm_tool.c/.h is the one source.
 *
 * Struct marshalling follows the rule 136.4 established: the compiler gives a
 * struct one i64 slot per field, so every value handed back to toke is built
 * that way rather than being a raw C struct the consumer would read at the
 * wrong offsets (127.86).
 */
#include "llm_tool.h"
#include "tk_array.h"
#include "strbuf.h"   /* 135.24: strbuf_append_json_unescape */
#include "tk_strhdr.h" /* 127.295: tks_strdup */
#include "tk_runtime.h"   /* 138.50: the handle registry ($llmclient) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */

/* 114.53/114.54/127.67: report failure through this flag and still return the
 * real value; defined in tk_runtime.c. */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

/* ── toke struct slot layouts (one i64 per field, in declaration order) ──
 *   $toolparam { name; type; desc; required }
 *   $tooldecl  { name; desc; params }
 *   $toolcall  { name; args; id }
 *   $toolresult{ id; content; error }
 *   $llmmsg    { role; content }
 *   $llmresp   { content; tokensin; tokensout; model }
 */
static const char *slot_str(int64_t block, int idx) {
    if (!block) return NULL;
    int64_t v = ((int64_t *)(intptr_t)block)[idx];
    return v ? (const char *)(intptr_t)v : NULL;
}
static int64_t slot_i64(int64_t block, int idx) {
    if (!block) return 0;
    return ((int64_t *)(intptr_t)block)[idx];
}

/*
 * 127.295 (S1 step 5, tranche 7): every string this module hands toke is
 * sealed -- it carries its length in the header tk_strlen reads in O(1).
 *
 * llm_tool.c's results stay malloc'd: its C callers (llm_submitresult,
 * llm_parallel_tool_calls, llm_agentic_loop, the C API's users) free them,
 * and a free() of a sealed string aborts.  So the seal happens HERE, at the
 * boundary: seal_take() makes the sealed copy toke keeps and frees the
 * malloc'd original, so nothing is left behind.  The copy is measured with
 * tk_strlen (strlen for the malloc'd original), so a decoded \u0000 ends
 * the string exactly where every C reader already saw it end.  If the
 * sealed copy cannot be made, the original is handed over instead -- an
 * unsealed string is read correctly, only with strlen.
 */
static char *seal_take(char *s) {
    if (!s) return NULL;
    char *t = tks_strdup(s);
    if (!t) return s;
    free(s);
    return t;
}

/*
 * The tools a client carries.
 *
 * llm.withtools is documented to return a NEW client carrying the tool
 * declarations, leaving the original unchanged, while TkLlmClient has no
 * tools field and llm_chatwithtools takes the tools as separate arguments.
 * So withtools allocates a fresh TkLlmClient and records the tools against
 * that handle here.
 *
 * This is a lookup keyed by the handle the caller passes, NOT an implicit
 * default of the kind 136.18 and 136.19 are about: two clients with two
 * different tool sets both work, and a client with no entry simply carries no
 * tools. Entries are never reused, matching toke's leak-forever model.
 */
#define TK_TOOLCLIENT_MAX 64
typedef struct {
    const TkLlmClient *client;
    TkToolDecl        *tools;
    uint64_t           ntools;
} ToolBinding;
static ToolBinding g_bindings[TK_TOOLCLIENT_MAX];
static int         g_nbindings = 0;

static const ToolBinding *binding_for(const TkLlmClient *c) {
    for (int i = 0; i < g_nbindings; i++)
        if (g_bindings[i].client == c) return &g_bindings[i];
    return NULL;
}

/* decode @(tooldecl) into a TkToolDecl array. Returns NULL for an empty set. */
static TkToolDecl *decode_tools(int64_t arr, uint64_t *out_n) {
    *out_n = 0;
    int64_t n = tk_arr_len(arr);
    if (n <= 0) return NULL;
    int64_t *elems = (int64_t *)(intptr_t)arr;
    TkToolDecl *tools = (TkToolDecl *)calloc((size_t)n, sizeof(TkToolDecl));
    if (!tools) return NULL;
    for (int64_t i = 0; i < n; i++) {
        int64_t d = elems[i];
        tools[i].name        = slot_str(d, 0);
        tools[i].description = slot_str(d, 1);
        int64_t parr = slot_i64(d, 2);
        int64_t np = tk_arr_len(parr);
        if (np > 0) {
            TkToolParam *ps = (TkToolParam *)calloc((size_t)np, sizeof(TkToolParam));
            if (ps) {
                int64_t *pe = (int64_t *)(intptr_t)parr;
                for (int64_t j = 0; j < np; j++) {
                    ps[j].name        = slot_str(pe[j], 0);
                    ps[j].type        = slot_str(pe[j], 1);
                    ps[j].description = slot_str(pe[j], 2);
                    ps[j].required    = slot_i64(pe[j], 3) ? 1 : 0;
                }
                tools[i].params  = ps;
                tools[i].nparams = (uint64_t)np;
            }
        }
    }
    *out_n = (uint64_t)n;
    return tools;
}

/* Free what decode_tools() allocated: the param arrays and the array itself.
 * The strings are borrowed from the toke values (slot_str), not owned. */
static void free_tools(TkToolDecl *tools, uint64_t n) {
    if (!tools) return;
    for (uint64_t i = 0; i < n; i++) free(tools[i].params);
    free(tools);
}

/* decode @(llmmsg) into a TkLlmMsg array. */
static TkLlmMsg *decode_msgs(int64_t arr, uint64_t *out_n) {
    *out_n = 0;
    int64_t n = tk_arr_len(arr);
    if (n <= 0) return NULL;
    int64_t *elems = (int64_t *)(intptr_t)arr;
    TkLlmMsg *msgs = (TkLlmMsg *)calloc((size_t)n, sizeof(TkLlmMsg));
    if (!msgs) return NULL;
    for (int64_t i = 0; i < n; i++) {
        const char *role = slot_str(elems[i], 0);
        const char *body = slot_str(elems[i], 1);
        msgs[i].role    = role ? role : "user";
        msgs[i].content = body ? body : "";
    }
    *out_n = (uint64_t)n;
    return msgs;
}

/*
 * args_pairs — the `args` field of a $toolcall: @(@($str)), one two-element
 * array per key in the model's arguments JSON object.
 *
 * The C side carries the arguments as a raw JSON string, so this walks the
 * top-level "key": value pairs and emits each key with its value rendered as
 * text (a string value decoded, anything else -- a nested object or array
 * included, whole (135.26) -- verbatim). An unparseable or
 * absent object gives a zero-length array rather than a null the consumer
 * would then index.
 *
 * 135.24 item 3: keys and string values are DECODED with
 * strbuf_append_json_unescape, the decoder llm_tool.c and tk_json_parse use.
 * They were copied verbatim, so an argument "a\"b" reached the program as
 * the four bytes a\"b, "line\nbreak" kept its backslash-n, and "\u00e9"
 * stayed six characters.  The walker also allocated its zero-length array
 * up front and dropped it whenever the object had keys; it is now allocated
 * only when it is the result.
 */
/*
 * json_value_end — the end of the non-string JSON value starting at p: the
 * first ',' or '}' (or '\0') outside any nested object or array and outside
 * any string (135.26 item 3).  The walker used to stop at the first ',' or
 * '}' anywhere, so an argument {"a":{"x":1,"y":2}} reached the program as
 * a = `{"x":1` followed by a bogus pair from the rest.  A nested object or
 * array is now handed over whole, as its JSON text.
 */
static const char *json_value_end(const char *p) {
    int depth = 0;
    while (*p) {
        char c = *p;
        if (c == '"') {                       /* a string inside a nested value */
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p) p++;
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { if (depth == 0) break; depth--; }
        else if (c == ',' && depth == 0) break;
        p++;
    }
    return p;
}

static int64_t args_pairs(const char *json) {
    const char *p = json ? strchr(json, '{') : NULL;
    if (!p) return tk_arr_alloc(0, 0);
    p++;

    /* two passes: count, then build, so the array header is right first time */
    int64_t count = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *q = p;
        int64_t idx = 0;
        int64_t h = 0;
        if (pass == 1) {
            h = tk_arr_alloc(count, count);
            if (!h) return tk_arr_alloc(0, 0);
        }
        while (*q && *q != '}') {
            while (*q && *q != '"' && *q != '}') q++;
            if (*q != '"') break;
            q++;
            TkStrBuf kb = TK_STRBUF_INIT, vb = TK_STRBUF_INIT;
            if (strbuf_append_json_unescape(&kb, &q) != 0) { strbuf_free(&kb); break; }
            while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == '"') {
                q++;
                (void)strbuf_append_json_unescape(&vb, &q);   /* unterminated: what there is */
            } else {
                const char *vs = q, *ve;
                q = json_value_end(q);
                ve = q;
                while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' ||
                                   ve[-1] == '\n' || ve[-1] == '\r')) ve--;
                strbuf_appendn(&vb, vs, (size_t)(ve - vs));
            }
            if (pass == 0) {
                count++;
                strbuf_free(&kb); strbuf_free(&vb);
            } else {
                int64_t pair = idx < count ? tk_arr_alloc(2, 2) : 0;
                char *k = pair ? strbuf_take(&kb) : NULL;
                char *v = pair ? strbuf_take(&vb) : NULL;
                strbuf_free(&kb); strbuf_free(&vb);
                if (!pair || !k || !v) { free(k); free(v); break; }
                k = seal_take(k);                         /* 127.295 */
                v = seal_take(v);
                ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)k;
                ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)v;
                ((int64_t *)(intptr_t)h)[idx++] = pair;
            }
            while (*q == ',' || *q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        }
        if (pass == 1) { tk_arr_setlen(h, idx); return h; }
        if (count == 0) break;
    }
    return tk_arr_alloc(0, 0);
}

/* build a $toolcall block { name; args; id } */
static int64_t toolcall_block(const TkToolCall *tc);

/* 135.3j: the $toolcall for the FIRST call of a successful parse, taking
 * ownership of the whole ToolCallResult.  Both callers used to return
 * toolcall_block(&r.calls[0]) and drop everything else: calls[0].args_json
 * (args_pairs copies out of it), every string of calls[1..n-1], and the
 * calls array itself -- a leak on every successful tool-call parse.  The
 * block keeps calls[0]'s name and id (the toke program owns them now);
 * the rest is freed here. */
static int64_t toolcall_block_take(ToolCallResult *r) {
    /* 127.295: the block holds sealed copies of calls[0]'s name and id
     * (seal_take frees the malloc'd originals); when there is no block the
     * originals are freed here. */
    TkToolCall c0 = r->calls[0];
    int64_t h = toolcall_block(&c0);
    if (h) {
        int64_t *b = (int64_t *)(intptr_t)h;
        if (c0.tool_name) b[0] = (int64_t)(intptr_t)seal_take((char *)c0.tool_name);
        if (c0.call_id)   b[2] = (int64_t)(intptr_t)seal_take((char *)c0.call_id);
    } else {                                    /* nothing handed out: free all */
        free((char *)r->calls[0].tool_name);
        free((char *)r->calls[0].call_id);
    }
    free((char *)r->calls[0].args_json);
    for (uint64_t i = 1; i < r->ncalls; i++) {
        free((char *)r->calls[i].tool_name);
        free((char *)r->calls[i].call_id);
        free((char *)r->calls[i].args_json);
    }
    free(r->calls);
    free((char *)r->err_msg);
    r->calls = NULL; r->ncalls = 0; r->err_msg = NULL;
    return h;
}

static int64_t toolcall_block(const TkToolCall *tc) {
    int64_t *b = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!b) return 0;
    b[0] = (int64_t)(intptr_t)(tc && tc->tool_name ? tc->tool_name : "");
    b[1] = args_pairs(tc ? tc->args_json : NULL);
    b[2] = (int64_t)(intptr_t)(tc && tc->call_id ? tc->call_id : "");
    return (int64_t)(intptr_t)b;
}

/* build a $llmresp block { content; tokensin; tokensout; model } */
static int64_t llmresp_block(const TkLlmResp *r, const char *model) {
    int64_t *b = (int64_t *)malloc(4 * sizeof(int64_t));
    if (!b) return 0;
    b[0] = (int64_t)(intptr_t)(r && r->content ? r->content : "");
    b[1] = r ? (int64_t)r->input_tokens : 0;
    b[2] = r ? (int64_t)r->output_tokens : 0;
    b[3] = (int64_t)(intptr_t)(model ? model : "");
    return (int64_t)(intptr_t)b;
}

/* ── the published surface ────────────────────────────────────────────── */

/* 138.50: only a client llm.client or withtools made is read (llm_glue.c
 * says why: a $llmclient record literal holds two of its slots); anything
 * else is answered as the null client is. */
static TkLlmClient *tool_client_in(int64_t v) { return (TkLlmClient *)tk_handle_addr(TK_HK_LLMCLIENT, v); }

int64_t tk_llmtool_withtools_w(int64_t client, int64_t tools_arr) {
    const TkLlmClient *src = tool_client_in(client);   /* 138.50 */
    if (!src) { tk_current_error = 1; return 0; }
    TkLlmClient *copy = (TkLlmClient *)malloc(sizeof(TkLlmClient));
    if (!copy) { tk_current_error = 1; return 0; }
    *copy = *src;                       /* the original is left unchanged */
    if (!tk_handle_adopt(TK_HK_LLMCLIENT, copy)) { free(copy); tk_current_error = 1; return 0; }

    uint64_t ntools = 0;
    TkToolDecl *tools = decode_tools(tools_arr, &ntools);
    if (g_nbindings < TK_TOOLCLIENT_MAX) {
        g_bindings[g_nbindings].client = copy;
        g_bindings[g_nbindings].tools  = tools;
        g_bindings[g_nbindings].ntools = ntools;
        g_nbindings++;
    } else {
        /* 135.3c: with the binding table full the decoded declarations were
         * dropped without being freed (clang unix.Malloc). */
        free_tools(tools, ntools);
    }
    tk_current_error = 0;
    return (int64_t)(intptr_t)copy;
}

int64_t tk_llmtool_chatwithtools_w(int64_t client, int64_t msgs_arr) {
    TkLlmClient *c = tool_client_in(client);   /* 138.50 */
    if (!c) { tk_current_error = 1; return 0; }
    const ToolBinding *b = binding_for(c);
    uint64_t nmsgs = 0;
    TkLlmMsg *msgs = decode_msgs(msgs_arr, &nmsgs);

    ToolCallResult r = llm_chatwithtools(c, msgs, nmsgs,
                                         b ? b->tools : NULL,
                                         b ? b->ntools : 0);
    free(msgs);
    if (r.is_err || r.ncalls == 0 || !r.calls) {
        free((char *)r.err_msg);   /* 135.3c: heap-owned; was dropped */
        free(r.calls);
        tk_current_error = 1; return 0;
    }
    tk_current_error = 0;
    return toolcall_block_take(&r);
}

int64_t tk_llmtool_submitresult_w(int64_t client, int64_t msgs_arr, int64_t result) {
    TkLlmClient *c = tool_client_in(client);   /* 138.50 */
    if (!c || !result) { tk_current_error = 1; return 0; }
    const ToolBinding *b = binding_for(c);
    uint64_t nmsgs = 0;
    TkLlmMsg *msgs = decode_msgs(msgs_arr, &nmsgs);

    TkToolResult tr;
    tr.call_id     = slot_str(result, 0);
    tr.result_json = slot_str(result, 1);

    TkLlmResp resp = llm_submitresult(c, msgs, nmsgs,
                                      b ? b->tools : NULL, b ? b->ntools : 0,
                                      &tr, 1);
    free(msgs);
    if (resp.is_err || !resp.content) { tk_current_error = 1; return 0; }
    tk_current_error = 0;
    return llmresp_block(&resp, c->model);
}

int64_t tk_llmtool_parsetoolcalls_w(int64_t raw) {
    if (!raw) { tk_current_error = 1; return 0; }
    ToolCallResult r = llm_parse_tool_calls((const char *)(intptr_t)raw);
    if (r.is_err || r.ncalls == 0 || !r.calls) {
        free((char *)r.err_msg);   /* 135.3c: heap-owned; was dropped */
        free(r.calls);
        tk_current_error = 1; return 0;
    }
    tk_current_error = 0;
    return toolcall_block_take(&r);
}

int64_t tk_llmtool_resultmsgs_w(int64_t results_arr) {
    int64_t n = tk_arr_len(results_arr);
    if (n <= 0) return tk_arr_alloc(0, 0);
    int64_t *elems = (int64_t *)(intptr_t)results_arr;

    TkToolResult *rs = (TkToolResult *)calloc((size_t)n, sizeof(TkToolResult));
    if (!rs) return tk_arr_alloc(0, 0);
    for (int64_t i = 0; i < n; i++) {
        rs[i].call_id     = slot_str(elems[i], 0);
        rs[i].result_json = slot_str(elems[i], 1);
    }

    uint64_t nout = 0;
    TkLlmMsg *msgs = llm_tool_result_msgs(rs, (uint64_t)n, &nout);
    free(rs);
    if (!msgs || nout == 0) { free(msgs); return tk_arr_alloc(0, 0); }

    int64_t h = tk_arr_alloc((int64_t)nout, (int64_t)nout);
    if (!h) { free(msgs); return tk_arr_alloc(0, 0); }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < nout; i++) {
        int64_t *m = (int64_t *)malloc(2 * sizeof(int64_t));
        if (!m) { tk_arr_setlen(h, (int64_t)i); break; }
        /* 127.295: sealed copies; seal_take frees llm_tool_result_msgs'
         * malloc'd role and content (they used to be handed over and kept
         * forever as malloc'd strings). */
        m[0] = (int64_t)(intptr_t)(msgs[i].role    ? seal_take((char *)msgs[i].role)    : "tool");
        m[1] = (int64_t)(intptr_t)(msgs[i].content ? seal_take((char *)msgs[i].content) : "");
        msgs[i].role = msgs[i].content = NULL;
        slots[i] = (int64_t)(intptr_t)m;
    }
    /* a malloc failure above stops the copy: free what was not handed out */
    for (uint64_t i = 0; i < nout; i++) {
        free((char *)msgs[i].role);
        free((char *)msgs[i].content);
    }
    free(msgs);
    return h;
}
