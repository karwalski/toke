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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
 * text (quotes stripped for strings, verbatim otherwise). An unparseable or
 * absent object gives a zero-length array rather than a null the consumer
 * would then index.
 */
static int64_t args_pairs(const char *json) {
    int64_t empty = tk_arr_alloc(0, 0);
    if (!json) return empty;
    const char *p = strchr(json, '{');
    if (!p) return empty;
    p++;

    /* two passes: count, then build, so the array header is right first time */
    int64_t count = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *q = p;
        int64_t idx = 0;
        int64_t h = 0;
        if (pass == 1) {
            h = tk_arr_alloc(count, count);
            if (!h) return empty;
        }
        while (*q && *q != '}') {
            while (*q && *q != '"' && *q != '}') q++;
            if (*q != '"') break;
            const char *ks = ++q;
            while (*q && *q != '"') q++;
            if (*q != '"') break;
            const char *ke = q++;
            while (*q == ' ' || *q == ':' || *q == '\t') q++;
            const char *vs, *ve;
            if (*q == '"') {
                vs = ++q;
                while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; }
                ve = q;
                if (*q == '"') q++;
            } else {
                vs = q;
                while (*q && *q != ',' && *q != '}') q++;
                ve = q;
                while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            }
            if (pass == 0) {
                count++;
            } else {
                int64_t pair = tk_arr_alloc(2, 2);
                if (!pair) break;
                char *k = (char *)malloc((size_t)(ke - ks) + 1);
                char *v = (char *)malloc((size_t)(ve - vs) + 1);
                if (!k || !v) { free(k); free(v); break; }
                memcpy(k, ks, (size_t)(ke - ks)); k[ke - ks] = '\0';
                memcpy(v, vs, (size_t)(ve - vs)); v[ve - vs] = '\0';
                ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)k;
                ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)v;
                if (idx < count) ((int64_t *)(intptr_t)h)[idx++] = pair;
            }
            while (*q == ',' || *q == ' ' || *q == '\t') q++;
        }
        if (pass == 1) { tk_arr_setlen(h, idx); return h; }
        if (count == 0) return empty;
    }
    return empty;
}

/* build a $toolcall block { name; args; id } */
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

int64_t tk_llmtool_withtools_w(int64_t client, int64_t tools_arr) {
    if (!client) { tk_current_error = 1; return 0; }
    const TkLlmClient *src = (const TkLlmClient *)(intptr_t)client;
    TkLlmClient *copy = (TkLlmClient *)malloc(sizeof(TkLlmClient));
    if (!copy) { tk_current_error = 1; return 0; }
    *copy = *src;                       /* the original is left unchanged */

    uint64_t ntools = 0;
    TkToolDecl *tools = decode_tools(tools_arr, &ntools);
    if (g_nbindings < TK_TOOLCLIENT_MAX) {
        g_bindings[g_nbindings].client = copy;
        g_bindings[g_nbindings].tools  = tools;
        g_bindings[g_nbindings].ntools = ntools;
        g_nbindings++;
    }
    tk_current_error = 0;
    return (int64_t)(intptr_t)copy;
}

int64_t tk_llmtool_chatwithtools_w(int64_t client, int64_t msgs_arr) {
    if (!client) { tk_current_error = 1; return 0; }
    TkLlmClient *c = (TkLlmClient *)(intptr_t)client;
    const ToolBinding *b = binding_for(c);
    uint64_t nmsgs = 0;
    TkLlmMsg *msgs = decode_msgs(msgs_arr, &nmsgs);

    ToolCallResult r = llm_chatwithtools(c, msgs, nmsgs,
                                         b ? b->tools : NULL,
                                         b ? b->ntools : 0);
    free(msgs);
    if (r.is_err || r.ncalls == 0 || !r.calls) { tk_current_error = 1; return 0; }
    tk_current_error = 0;
    return toolcall_block(&r.calls[0]);
}

int64_t tk_llmtool_submitresult_w(int64_t client, int64_t msgs_arr, int64_t result) {
    if (!client || !result) { tk_current_error = 1; return 0; }
    TkLlmClient *c = (TkLlmClient *)(intptr_t)client;
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
    if (r.is_err || r.ncalls == 0 || !r.calls) { tk_current_error = 1; return 0; }
    tk_current_error = 0;
    return toolcall_block(&r.calls[0]);
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
        m[0] = (int64_t)(intptr_t)(msgs[i].role    ? msgs[i].role    : "tool");
        m[1] = (int64_t)(intptr_t)(msgs[i].content ? msgs[i].content : "");
        slots[i] = (int64_t)(intptr_t)m;
    }
    free(msgs);
    return h;
}
