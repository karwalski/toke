#!/usr/bin/env bash
# C2111_record_prefix_literals_refused.sh — the rest of story 138.50: a
# record literal of a tki-record-prefix std type ($htmlnode, $httpstream,
# $canvas) is answered as the null value instead of being read past its end.
#
# THE DEFECT.  These .tki records are only the leading slots of a larger C
# struct (html.h, http.h, canvas.h carry `tki-record-prefix` markers); the
# private fields follow.  --check accepts a record literal, which holds the
# declared slots only, so html.append linked a literal node through its
# child/sibling pointers, http.streamnext read a literal stream's socket and
# buffer, and canvas.fillrect / filltext appended to a literal canvas's op
# buffer -- all from memory past the literal.
#
# THE FIX.  html.h1 / html.table, http.stream and canvas.new register what
# they make in the runtime's handle registry (tk_runtime.c, 138.45); the
# calls that take one look it up first and answer anything else as the null
# value: html.append skips the node, http.streamnext is $err "null stream"
# (it was a bare 0 with the error slot untouched), canvas.fillrect /
# filltext / tohtml return the null canvas.  As for $llmclient (C2064) a
# literal is not given defaults.
#
# Negative control: TKC=<base 6ec0044d tree>/tkc -- htmllit renders the
# literal node's <p> or crashes, streamlit and canvaslit crash.
#
# Story: 138.50
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2111_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2111: tki-record-prefix literals are the null value (138.50)"
echo "----------------------------------------------------------------------"

run() {
    local name="$1" want="$2" got rc
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build: $(grep -m1 error "${name}.log" | cut -c1-200)"; return
    fi
    got="$(./"${name}" --allow-all 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; fi
}

cat > htmllit.tk <<'EOF'
m=ht;
i=io:std.io;
i=html:std.html;
f=main():i64{
  let d=html.doc();
  html.append(d;html.h1("real"));
  html.append(d;$htmlnode{tag:"p";attrs:@();content:"lit"});
  let n=5;
  html.append(d;n as $htmlnode);
  io.println(html.render(d));
  <0
};
EOF
run htmllit '<!DOCTYPE html>
<html>
<head>
</head>
<body>
<h1>real</h1>
</body>
</html>'

cat > streamlit.tk <<'EOF'
m=h;
i=io:std.io;
i=http:std.http;
f=main():i64{
  let ls=$httpstream{id:1;open:true};
  io.println("literal: \(mt http.streamnext(ls){$ok:b "OK";$err:e e.msg})");
  let n=5;
  let fs=n as $httpstream;
  io.println("forged: \(mt http.streamnext(fs){$ok:b "OK";$err:e e.msg})");
  <0
};
EOF
run streamlit 'literal: http.streamnext: null stream
forged: http.streamnext: null stream'

cat > canvaslit.tk <<'EOF'
m=cv;
i=io:std.io;
i=canvas:std.canvas;
f=main():i64{
  let c=canvas.new("c";10;10);
  let c2=canvas.fillrect(c;0.0;0.0;1.0;1.0;"red");
  let c3=canvas.filltext(c2;"hi";1.0;1.0;"10px sans";"blue");
  io.println("real: html=\(canvas.tohtml(c3).len>0) w=\(c3.width)");
  let lit=$canvas{id:"l";width:4;height:4;ops:@()};
  let r=canvas.fillrect(lit;0.0;0.0;1.0;1.0;"red");
  let t=canvas.filltext(lit;"x";0.0;0.0;"f";"red");
  let n=5;
  let fc=n as $canvas;
  let r2=canvas.fillrect(fc;0.0;0.0;1.0;1.0;"red");
  io.println("literal and forged answered");
  <0
};
EOF
run canvaslit 'real: html=true w=10
literal and forged answered'

echo "----------------------------------------------------------------------"
echo "C2111: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
