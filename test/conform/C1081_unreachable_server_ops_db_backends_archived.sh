#!/usr/bin/env bash
# C1081_unreachable_server_ops_db_backends_archived.sh — server_ops.c,
# db_postgres.c and db_mysql.c are gone, and nothing needed them (story
# 121.71, split from 121.53 (4)).
#
# THE FINDING, measured at main 5eb15d9a:
#   server_ops.c  20 external functions (proc_healthz, proc_readyz,
#                 proc_config_test, proc_event_loop_*, phase / grace-period /
#                 hook setters, reuseport helpers) and no header.  No other
#                 stdlib source, glue wrapper or .tki export named any of
#                 them; tkc linked the file into every std.http program
#                 (src/stdlib_deps.c "http" row, llvm.c find_stdlib_sources)
#                 as dead weight.  Its only users were tests of itself.
#   db_postgres.c / db_mysql.c  wholly inside #ifdef TK_HAVE_LIBPQ /
#                 TK_HAVE_MYSQL, which no build defines; they exported a
#                 DbBackend vtable that db.c / db_glue.c never dispatched
#                 (db.c talks to SQLite directly).  Not in any link set.
#   Downstream (toke-ooke main 783981c, toke-website main 2c266e9,
#   toke-test-programs main ecb140e4, toke-mcp main 78731fe): no source
#   names a file or symbol of the three.
#
# NOW: the three files, the DbBackend block of db.h, their skiplist entries
# and the link-set entries are archived/removed (the files and their own
# tests are in the local archive with a MANIFEST).  This asserts:
#   1-3  each file is absent from src/stdlib
#   4    no manifest (stdlib_deps.c, llvm.c, Makefile) names any of them
#   5    tkc --emit-deps for a std.http + std.db program names none of them
#   6    no stdlib source, header or .tki names one of their symbols
#   7    a program importing std.http and std.db still links and runs:
#        db.open(":memory:") + db.exec work, exit 0 (the link set is whole)
#
# Negative control: C1081_STDLIB=<main 5eb15d9a's tree root> (src/stdlib and
# stdlib beside it) fails 1-4 and 6 against that tree (5 reads the link set
# compiled into the tkc under test, so it follows the binary, not the tree;
# 7 still links and runs -- the archived files were never needed).
#
# Story: 121.71
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
T="${C1081_STDLIB:-${REPO_ROOT}}"
S="${T}/src/stdlib"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1081_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
echo "C1081: unreachable server_ops / db backends are archived (121.71)"
echo "-----------------------------------------------------------------"

FILES="server_ops.c db_postgres.c db_mysql.c"
for f in ${FILES}; do
    if [ -e "${S}/${f}" ]; then fail "src/stdlib/${f} still exists"
    else pass "src/stdlib/${f} is absent"; fi
done

named="$(/usr/bin/grep -a -n -E '\b(server_ops|db_postgres|db_mysql)\.c\b' \
    "${T}/src/stdlib_deps.c" "${T}/src/llvm.c" "${T}/Makefile" 2>/dev/null \
    | /usr/bin/grep -a -v -E '^[^:]+:[0-9]+:[[:space:]]*(#|/?\*|//)' | head -3)"
if [ -n "${named}" ]; then fail "a link manifest still names an archived file: ${named}"
else pass "no link manifest names server_ops.c, db_postgres.c or db_mysql.c"; fi

printf 'm=main;i=http:std.http;i=db:std.db;i=io:std.io;\nf=main():i64{<0};\n' > "${WORK}/d.tk"
deps="$(TKC_STDLIB_DIR="${S}" "${TKC}" --emit-deps "${WORK}/d.tk" 2>&1)"
if [ -z "${deps}" ] || ! printf '%s\n' "${deps}" | /usr/bin/grep -a -q '/http\.c$'; then
    fail "tkc --emit-deps printed no http.c (control): ${deps:0:200}"
elif printf '%s\n' "${deps}" | /usr/bin/grep -a -q -E '/(server_ops|db_postgres|db_mysql)\.c$'; then
    fail "tkc --emit-deps still links an archived file"
else
    pass "tkc --emit-deps (std.http + std.db) names none of them"
fi

SYMS='proc_(set_grace_period|set_startup_hook|set_shutdown_hook|config_test|healthz|readyz|set_phase|get_phase|get_grace_period|run_startup_hook|run_shutdown_hook|event_loop_new|event_loop_add|event_loop_remove|event_loop_poll|event_loop_free|set_nonblocking|reuseport_available|enable_reuseport)|ProcConfig|TkEventLoop|ProcHook|DbBackend|db_postgres_backend|db_mysql_backend|TK_HAVE_LIBPQ|TK_HAVE_MYSQL'
uses="$(cd "${T}" && /usr/bin/grep -a -n -E "\\b(${SYMS})\\b" -- src/stdlib/*.c src/stdlib/*.h stdlib/*.tki 2>/dev/null \
    | /usr/bin/grep -a -v -E '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)' | head -3)"
if [ -n "${uses}" ]; then fail "the stdlib still names an archived symbol: ${uses}"
else pass "no stdlib source, header or .tki names a symbol of the archived files"; fi

cat > "${WORK}/p.tk" <<'EOF'
m=main;i=http:std.http;i=db:std.db;i=io:std.io;
f=main():i64{
  let h = mt db.open(":memory:") {$ok:v v;$err:e < 2};
  let n = mt db.exec("CREATE TABLE t(a TEXT)"; @()) {$ok:v v;$err:e < 3};
  io.println("ok");
  <0
};
EOF
if TKC_STDLIB_DIR="${S}" "${TKC}" --out "${WORK}/p" "${WORK}/p.tk" >"${WORK}/b.txt" 2>&1; then
    out="$("${WORK}/p" --allow-read --allow-write 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${out}" = "ok" ]; then pass "a std.http + std.db program links and runs (exit 0)"
    else fail "the std.http + std.db program ran: rc=${rc} out=${out:0:200}"; fi
else
    fail "a std.http + std.db program does not link: $(grep -o '"message":"[^"]*"' "${WORK}/b.txt" | head -1)"
fi

echo ""
echo "C1081: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
