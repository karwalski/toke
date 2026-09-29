#!/usr/bin/env bash
# C389_tki_returned_records_marked.sh — every record a .tki function returns
# has a marked C struct, and every marked struct has its record's layout
# (story 135.30).
#
# THE DEFECT CLASS.  tkc reads `x.f` as 8-byte slot <index of f in the
# .tki> of the block a record handle points at.  138.18's gate
# (scripts/check_tki_layout.py, C361) checked only the structs someone had
# marked `/* tki-record: mod.type */`; nothing required the mark, so the
# glue went on handing toke raw C structs of other shapes.  llm.client
# returned TkLlmClient {base_url; api_key; model; ...} as the $llmclient
# {provider; model}: `c.model` printed the API key.  canvas, chart,
# dashboard, db's Row, html, i18n, ml, svg, template, tls and ws had the
# same fault (C380-C387).
#
# THE GATE NOW.  A record with fields that a .tki function returns --
# directly, or as the elements of an array it returns -- must be marked;
# records in other territory not marked yet are listed in the script's
# PENDING table (reported, not failed; a stale entry fails).
#
# WHAT IS ASSERTED (each on a copy of the tree, never the tree itself):
#   1. the tree: OK, and the structs of all twelve 135.30 modules are checked
#   2. the marker removed from TkLlmClient: FAIL naming llm.llmclient
#   3. the pre-135.30 TkLlmClient (base_url; api_key; model first): FAIL --
#      the API-key layout
#   4. provider and model swapped in TkLlmClient: FAIL
#   5. two fields swapped in an embedded head (TkSvgDocHead): FAIL
#   6. canvas width/height back to two uint32_t: FAIL from the compiler
#   7. a new .tki function returning an unmarked record: FAIL naming it
#   8. a PENDING record that has been marked: FAIL (stale entry)
#   9. a zero-field record (svg's svgelem) needs no marker: still OK
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c389_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
echo "C389: returned .tki records are marked and match their C structs (135.30)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
fresh() {
    rm -rf "${WORK}/t"; mkdir -p "${WORK}/t/scripts" "${WORK}/t/src/stdlib" "${WORK}/t/stdlib"
    cp "${REPO_ROOT}/scripts/check_tki_layout.py" "${WORK}/t/scripts/"
    cp "${REPO_ROOT}"/src/stdlib/*.h "${WORK}/t/src/stdlib/"
    cp "${REPO_ROOT}"/stdlib/*.tki "${WORK}/t/stdlib/"
}
# edit FILE PY-EXPR : apply a Python str transform; fail loudly if it changed nothing
edit() {
    python3 - "$1" "$2" <<'PY' || { echo "  FAIL: injection into $1 did not apply"; exit 1; }
import sys
p, expr = sys.argv[1], sys.argv[2]
s = open(p).read()
t = eval(expr, {"s": s, "json": __import__("json")})
if t == s:
    sys.exit(1)
open(p, "w").write(t)
PY
}
gate() {  # "<rc> <status word>"
    local out rc
    out="$(python3 "${WORK}/t/scripts/check_tki_layout.py" --list 2>&1)"; rc=$?
    echo "${out}" > "${WORK}/last.out"
    echo "${rc} $(printf '%s\n' "${out}" | tail -1 | cut -d' ' -f2)"
}
seen() { grep -c -- "$1" "${WORK}/last.out"; }
S="${WORK}/t/src/stdlib"

fresh
expect "1  the tree: OK" "$(gate)" "0 OK"
n=0
for r in llm.llmclient llm.llmresp canvas.canvas canvas.canvasop chart.chartspec \
         dashboard.dashwidget dashboard.dashboard db.Row html.htmlnode html.htmldoc \
         i18n.i18nbundle ml.linearmodel svg.svgdoc svg.svgstyle template.tmpl \
         template.tmplvars tls.tlsconn tls.tlsconfig tls.tlskeypair ws.wsconn ws.wsmsg; do
    grep -q "= ${r} (" "${WORK}/last.out" && n=$((n + 1))
done
expect "1b the 21 structs of the twelve modules are among those checked" "${n}" "21"

fresh
edit "${S}/llm.h" 's.replace("/* tki-record-prefix: llm.llmclient */\n", "", 1)'
expect "2  TkLlmClient unmarked: FAIL" "$(gate)" "1 FAIL"
expect "2b ... naming llm.llmclient and llm.client" \
    "$(seen 'llm.llmclient is returned by llm.client, llmtool.withtools but no struct')" "1"

fresh
edit "${S}/llm.h" 's.replace("    const char *provider;    /* slot 0: \"openai\", \"anthropic\", \"ollama\" or the URL\x27s host */\n    const char *model;       /* slot 1: e.g. \"gpt-4o\", \"claude-3-5-sonnet-20241022\" */\n    /* private: never reachable as a toke field */\n    const char *base_url;", "    const char *base_url;\n    const char *api_key;\n    const char *model;\n    const char *provider;\n    const char *base_url2;", 1).replace("    const char *api_key;     /* Bearer token, may be NULL for local */\n", "", 1)'
expect "3  the pre-135.30 TkLlmClient (api_key in slot 1): FAIL" "$(gate)" "1 FAIL"
expect "3b ... the names say so" "$(seen "TkLlmClient mirrors llm.llmclient but its fields are \['base_url', 'api_key'\]")" "1"

fresh
edit "${S}/llm.h" 's.replace("    const char *provider;    /* slot 0", "    const char *PLACEHOLDER;    /* slot 0", 1).replace("    const char *model;       /* slot 1", "    const char *provider;       /* slot 1", 1).replace("PLACEHOLDER", "model", 1)'
expect "4  provider and model swapped: FAIL" "$(gate)" "1 FAIL"

fresh
edit "${S}/svg.h" 's.replace("    char       *viewbox;    /* \"0 0 <width> <height>\", as svg_render writes it */\n    int64_t     elements;", "    int64_t     elements;\n    char       *viewbox;", 1)'
expect "5  TkSvgDocHead viewbox/elements swapped: FAIL" "$(gate)" "1 FAIL"

fresh
edit "${S}/canvas.h" 's.replace("    int64_t  width;     /* u32 */\n    int64_t  height;    /* u32 */", "    uint32_t width;\n    uint32_t height;", 1)'
expect "6  canvas width/height as two uint32_t: FAIL" "$(gate)" "1 FAIL"
expect "6b ... the compiler names the slot" "$(seen 'C layout: TkCanvas width is_slot_1 -- false')" "1"

fresh
edit "${WORK}/t/stdlib/canvas.tki" '(lambda d: (d["exports"].append({"kind": "type", "name": "canvasinfo", "fields": [{"name": "n", "type": "u64"}]}), d["exports"].append({"kind": "func", "name": "canvas.info", "params": ["canvas"], "return": "canvasinfo"}), json.dumps(d, indent=2))[2])(json.loads(s))'
expect "7  a new function returning an unmarked record: FAIL" "$(gate)" "1 FAIL"
expect "7b ... naming it" "$(seen 'canvas.canvasinfo is returned by canvas.info but no struct')" "1"

fresh
edit "${S}/canvas.h" 's.replace("/* tki-record: canvas.canvasop */", "/* tki-record: yaml.Yaml */\ntypedef struct {\n    int64_t     h;\n} TkYamlFake;\n\n/* tki-record: canvas.canvasop */", 1)'
expect "8  a PENDING record now marked: FAIL" "$(gate)" "1 FAIL"
expect "8b ... the entry is called stale" "$(seen 'PENDING lists yaml.Yaml, but it is marked now')" "1"

fresh
gate > /dev/null
expect "9  svg.svgelem (no fields) is returned but needs no marker" \
    "$(seen 'svg.svgelem')" "0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
