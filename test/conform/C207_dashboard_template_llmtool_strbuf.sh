#!/usr/bin/env bash
# C207_dashboard_template_llmtool_strbuf.sh — dashboard, template and
# llm_tool build their output in the shared TkStrBuf and escape every caller
# string for the context it lands in (story 122.13 (8)).
#
# THE DEFECTS (measured on main before 122.13 (8)).
#   dashboard_render snprintf'd each widget into char[512]/char[1024]: a long
#   title was cut mid-element.  Titles, ids, stat values and markdown went
#   into HTML raw (a title "</h2><script>..." injected markup), the widget id
#   into a '...' JavaScript string raw.  (A chart title "</script>..." was
#   already stopped by html_render's raw-text guard; checked here as a
#   guard.)  dashboard_export_json passed control bytes other than \n \r \t
#   through (invalid JSON) and wrote a NaN gauge as "nan".
#   llm_agentic_loop sized the tool-result message from the UNESCAPED call id,
#   so an id whose escaped form is longer was cut short by snprintf: the tool
#   message sent back to the model was not JSON.
#   template.c and llm_tool.c carried private buffer/escape copies; the
#   template checks below are regression guards (same output as before).
#
# WHAT THIS DRIVES (ASan+UBSan build of the modules):
#   - dashboard_render / dashboard_export_json with hostile titles, ids,
#     values, markdown and a 3000-byte title;
#   - tmpl_renderhtml escaping and tmpl_renderpage with a 20000-byte block
#     yielded three times, and (a counting-allocator build of template.c)
#     a page whose directive parse fails after a layout() and a block():
#     nothing may be left allocated (main leaked 3);
#   - llm_agentic_loop against a local mock server (python3) whose first
#     reply is a tool call with an id of 80 double quotes; the second request
#     is captured and its tool message content must parse as JSON carrying
#     the whole id.
#
# Negative control: C207_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's dashboard.c/llm_tool.c FAILs the dashboard and llm checks.
#
# Story: 122.13

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C207_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c207_XXXXXX")"
SRV_PID=""
trap '[ -n "${SRV_PID}" ] && kill "${SRV_PID}" 2>/dev/null; rm -rf "${WORK}"' EXIT

echo "C207: dashboard/template/llm_tool escape into TkStrBuf, nothing truncated"
echo "-------------------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (needed for the mock LLM server)"
    echo "CONFORM-SKIP: python3 not installed"
    exit 0
fi

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dashboard.h"
#include "chart.h"
#include "template.h"
#include "llm.h"
#include "llm_tool.h"
static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
static const char *tool_handler(const char *args) { (void)args; return strdup("{\"ok\":1}"); }
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) return 2;
    const char *dir = argv[1], *url = argv[2];

    /* ---- dashboard_render ---- */
    char longt[3001]; memset(longt, 'T', 3000); longt[3000] = 0; longt[2999] = 'Z';
    TkDashboard *d = dashboard_new("dash", 2);
    dashboard_add_stat(d, "s\" onmouseover=\"x", "</h2><script>alert(1)</script>", "<b>9</b>", "&u");
    dashboard_add_markdown(d, "m", "<img src=x onerror=alert(4)>");
    dashboard_add_gauge(d, "g", longt, 1.0, 0.0, 2.0);
    static const char *labs[] = { "a" };
    static const double vals[] = { 1.0 };
    TkDataset ds = { "set", vals, 1, NULL };
    StrArray la = { labs, 1 };
    TkChartSpec *spec = chart_bar(la, &ds, 1, "</script><script>alert(2)</script>");
    dashboard_addchart(d, "c');alert(3);//", "chart", spec, 0, 0, 1, 1);
    const char *html = dashboard_render(d);
    CHECK(html != NULL, "dashboard_render returns HTML");
    if (!html) html = "";
    CHECK(!strstr(html, "<script>alert(1)") && strstr(html, "&lt;/h2&gt;&lt;script&gt;alert(1)"),
          "a stat title cannot open a <script>: it is HTML-escaped");
    CHECK(!strstr(html, "onmouseover=\"x"), "a widget id cannot add an attribute");
    CHECK(!strstr(html, "<b>9</b>") && !strstr(html, "<img src=x"),
          "stat value and markdown content are text, not markup");
    CHECK(!strstr(html, "</script><script>alert(2)"),
          "a chart title cannot close the inline <script> element");
    CHECK(!strstr(html, "c');alert(3)"), "a chart id cannot leave its JavaScript string");
    const char *tz = strstr(html, longt);
    CHECK(tz && strncmp(tz + 3000, "</h2>", 5) == 0,
          "a 3000-byte title is written whole, element closed after it");

    /* ---- dashboard_export_json ---- */
    TkDashboard *e = dashboard_new("x", 1);
    dashboard_add_stat(e, "id", "a\001b\037c", "v", "u");
    dashboard_add_gauge(e, "g", "t", NAN, 0.0, INFINITY);
    const char *js = dashboard_export_json(e);
    CHECK(js && !strchr(js, '\001') && !strchr(js, '\037') && strstr(js, "a\\u0001b\\u001fc"),
          "export_json escapes every control byte (\\u0001, \\u001f)");
    CHECK(js && !strstr(js, "nan") && !strstr(js, "inf") && strstr(js, "\"value\":null"),
          "export_json writes NaN/Inf gauge values as null, not nan/inf");
    if (js) { FILE *f = fopen(argv[3], "w"); fputs(js, f); fclose(f); }

    /* ---- template ---- */
    TkTmpl *t = tmpl_compile("<p>{{v}}</p>");
    TkTmplVar var = { "v", "<b>&\"'</b>" };
    const char *r = t ? tmpl_renderhtml(t, &var, 1) : NULL;
    CHECK(r && strcmp(r, "<p>&lt;b&gt;&amp;&quot;&#39;&lt;/b&gt;</p>") == 0,
          "tmpl_renderhtml escapes & < > \" ' exactly as before");
    char p1[1024], p2[1024];
    snprintf(p1, sizeof p1, "%s/layout.tkt", dir);
    snprintf(p2, sizeof p2, "%s/page.tkt", dir);
    FILE *f = fopen(p1, "w");
    fputs("<html>{! yield(\"body\") !}|{! yield(\"body\") !}|{! yield(\"body\") !}</html>", f); fclose(f);
    f = fopen(p2, "w");
    fputs("{! layout(\"layout\") !}{! block(\"body\") !}", f);
    for (int i = 0; i < 20000; i++) fputc('a' + i % 26, f);
    fputs("{! end !}", f); fclose(f);
    const char *pg = tmpl_renderpage(p2, dir, NULL, 0);
    CHECK(pg && strlen(pg) == 6 + 3 * 20000 + 2 + 7 && strncmp(pg, "<html>abc", 9) == 0
             && strcmp(pg + strlen(pg) - 7, "</html>") == 0,
          "tmpl_renderpage yields a 20000-byte block three times, whole");

    /* ---- llm_agentic_loop tool-result message ---- */
    TkLlmClient *c = llm_client(url, "k", "m");
    TkTool tool = { "echo", "echo", NULL, tool_handler };
    const char *ans = llm_agentic_loop(c, NULL, "hi", &tool, 1, 3);
    CHECK(ans && strcmp(ans, "done") == 0, "agentic loop completes against the mock server");
    printf("  info: answer: %s\n", ans ? ans : "(null)");
    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

cat > "${WORK}/srv.py" <<'PY'
import http.server, json, sys, threading
out = sys.argv[1]
n = [0]
ID = '"' * 80
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        n[0] += 1
        with open(f"{out}/req{n[0]}.json", "wb") as f: f.write(body)
        if n[0] == 1:
            msg = {"role": "assistant", "content": "",
                   "tool_calls": [{"id": ID, "type": "function",
                                   "function": {"name": "echo", "arguments": "{}"}}]}
        else:
            msg = {"role": "assistant", "content": "done"}
        r = json.dumps({"choices": [{"message": msg, "finish_reason": "stop"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(r)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(r)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
with open(f"{out}/port", "w") as f: f.write(str(s.server_address[1]))
s.serve_forever()
PY

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${S}/dashboard.c" "${S}/chart.c" "${S}/html.c" "${S}/router.c" "${S}/ws.c" \
        "${S}/capabilities.c" \
        "${S}/template.c" "${S}/llm_tool.c" "${S}/llm.c" -lz -lm >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    echo "C207: FAIL (build)"
    exit 1
fi

mkdir -p "${WORK}/d"
python3 "${WORK}/srv.py" "${WORK}" &
SRV_PID=$!
for _ in $(seq 1 50); do [ -s "${WORK}/port" ] && break; sleep 0.1; done
if [ ! -s "${WORK}/port" ]; then echo "  FAIL: mock server did not start"; echo "C207: FAIL"; exit 1; fi
PORT="$(cat "${WORK}/port")"

ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" "${WORK}/d" "http://127.0.0.1:${PORT}" "${WORK}/export.json"
rc=$?

# The tool message the loop sent back must be JSON carrying the whole id,
# and the exported dashboard JSON must parse.
python3 - "${WORK}" <<'PY'
import json, sys, os
w = sys.argv[1]
ok = 0; bad = 0
def check(c, m):
    global ok, bad
    print(("  PASS: " if c else "  FAIL: ") + m)
    if c: ok += 1
    else: bad += 1
try:
    json.load(open(f"{w}/export.json")); good = True
except Exception as e:
    good = False
check(good, "dashboard_export_json output parses as JSON")
p = f"{w}/req2.json"
tool = None
if os.path.exists(p):
    try:
        req = json.load(open(p))
        tool = [m for m in req.get("messages", []) if m.get("role") == "tool"]
    except Exception:
        tool = None
content = None
if tool:
    try:
        content = json.loads(tool[-1]["content"])
    except Exception as e:
        print("  info: tool content does not parse: %r" % tool[-1]["content"][:200])
check(content is not None, "the tool-result message sent back to the model is valid JSON")
# 122.14: llm_parse_tool_calls now decodes the id, so the id sent back is
# exactly the one the model sent: 80 double quotes (it used to come back as
# 80 x backslash-quote, the escapes undecoded).  All of it must survive.
check(content is not None and content.get("tool_call_id") == '"' * 80
      and content.get("content") == {"ok": 1},
      "it carries the whole 80-quote call id, decoded, and the tool's result")
sys.exit(1 if bad else 0)
PY
prc=$?

# ---- tmpl_renderpage frees what a failed directive parse left behind ----
# A page whose layout() and first block() parse, followed by a block(" with
# no closing quote: parse_page_directives returns 0 and tmpl_renderpage
# leaked layout_name and the block (3 allocations; the analyzer's three
# unix.Malloc warnings at template.c's parse call, unmasked by the strbuf
# rewrite of apply_yields).  template.c is built with a counting allocator.
cat > "${WORK}/cnt.h" <<'C'
#include <stdlib.h>
#include <string.h>
void *cnt_malloc(size_t); void *cnt_calloc(size_t, size_t); void *cnt_realloc(void *, size_t);
void cnt_free(void *); char *cnt_strdup(const char *);
#define malloc(n)     cnt_malloc(n)
#define calloc(a, b)  cnt_calloc(a, b)
#define realloc(p, n) cnt_realloc(p, n)
#define free(p)       cnt_free(p)
#define strdup(s)     cnt_strdup(s)
C
cat > "${WORK}/cnt.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "template.h"
long cnt_live = 0;
void *cnt_malloc(size_t n) { void *p = malloc(n); if (p) cnt_live++; return p; }
void *cnt_calloc(size_t a, size_t b) { void *p = calloc(a, b); if (p) cnt_live++; return p; }
void *cnt_realloc(void *o, size_t n) { void *p = realloc(o, n); if (p && !o) cnt_live++; return p; }
void cnt_free(void *p) { if (p) cnt_live--; free(p); }
char *cnt_strdup(const char *s) { char *p = strdup(s); if (p) cnt_live++; return p; }
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    long before = cnt_live;
    const char *r = tmpl_renderpage(argv[1], argv[2], NULL, 0);
    long left = cnt_live - before;
    if (!r && left == 0) {
        printf("  PASS: a malformed block() after layout()/block(): NULL, nothing left allocated\n");
        return 0;
    }
    printf("  FAIL: a malformed block() after layout()/block(): %s, %ld allocation(s) left\n",
           r ? "non-NULL" : "NULL", left);
    return 1;
}
C
mkdir -p "${WORK}/tl"
printf '<p>{! yield("a") !}</p>' > "${WORK}/tl/base.tkt"
printf '{! layout("base") !}{! block("a") !}xx{! end !}{! block("bad' > "${WORK}/tl/page.tkt"
trc=1
if "${CC}" -std=c11 -D_GNU_SOURCE -g ${SAN} -iquote "${S}" -c -include "${WORK}/cnt.h" \
        -o "${WORK}/tmpl_cnt.o" "${S}/template.c" >"${WORK}/tcc.log" 2>&1 &&
   "${CC}" -std=c11 -D_GNU_SOURCE -g ${SAN} -iquote "${S}" -o "${WORK}/tl_t" \
        "${WORK}/cnt.c" "${WORK}/tmpl_cnt.o" >>"${WORK}/tcc.log" 2>&1; then
    ASAN_OPTIONS="detect_leaks=0" "${WORK}/tl_t" "${WORK}/tl/page.tkt" "${WORK}/tl"
    trc=$?
else
    echo "  FAIL: template leak driver does not build: $(head -c 600 "${WORK}/tcc.log")"
fi

echo ""
if [ "${rc}" -eq 0 ] && [ "${prc}" -eq 0 ] && [ "${trc}" -eq 0 ]; then echo "C207: PASS"; exit 0; fi
echo "C207: FAIL (driver ${rc}, wire ${prc}, template-leak ${trc})"
exit 1
