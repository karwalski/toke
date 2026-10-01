#!/usr/bin/env bash
# C660_log_output_reopens_after_rotation.sh -- after logrotate renames them,
# tk_log_reopen_files() reopens the tk_log_set_output file and the security
# stream's file at their paths (story 121.72).
#
# THE DEFECT, measured on main (437dc471): tk_log_reopen_files (121.53) --
# what every serving loop calls on SIGHUP -- reopened only the access and
# error logs.  log.c's g_outfile (tk_log_set_output) and g_secfile
# (tk_log_security_set_output) kept no path, so after "rename, then HUP" both
# went on writing into the renamed file forever.
#
# NOW: both sinks remember their path; tk_log_reopen_files reopens them there.
# The new file is opened before the old one is closed, so a path that cannot
# be reopened leaves the sink writing where it was (no line is lost), and a
# /dev/ path or a non-regular file is left alone, as for the access log.
#
# Neither sink is on std.log's .tki (log.h: C interface only), so this builds
# a small C harness against log.c.
#
# WHAT IS ASSERTED:
#   1. rename + reopen: the main log's post-rotation line is in the new file,
#      not the rotated one;
#   2. the same for the security stream;
#   3. unreopenable path (directory made read-only after the rename): the
#      post line still lands in the rotated file -- not lost, no crash;
#   4. tk_log_set_output(NULL) after a file: reopen is a no-op (stderr).
#
# Negative control: C660_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib, 1 and 2 fail (post lines in the rotated file).
#
# Story: 121.72
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
S="${C660_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c660_XXXXXX")"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT
echo "C660: tk_log_reopen_files reopens the set-output and security files (121.72)"
echo "-----------------------------------------------------------------------------"

cat > "${WORK}/h.c" <<'C'
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static const char *F[] = {"k", "v"};
int main(int argc, char **argv)
{
    const char *d = argv[1];
    char a[1024], a1[1024], s[1024], s1[1024];
    (void)argc;
    snprintf(a, sizeof a, "%s/app.log", d);   snprintf(a1, sizeof a1, "%s/app.log.1", d);
    snprintf(s, sizeof s, "%s/sec.log", d);   snprintf(s1, sizeof s1, "%s/sec.log.1", d);
    if (!tk_log_set_output(a) || !tk_log_security_set_output(s)) return 2;
    tk_log_info("pre-main", F, 2);
    tk_log_security("pre-sec", F, 2);
    if (rename(a, a1) != 0 || rename(s, s1) != 0) return 3;
    if (strcmp(argv[2], "ro") == 0) chmod(d, 0555);
    tk_log_reopen_files();
    tk_log_info("post-main", F, 2);
    tk_log_security("post-sec", F, 2);
    tk_log_set_output(NULL);
    tk_log_security_set_output(NULL);
    tk_log_reopen_files();          /* nothing to reopen: must not crash */
    chmod(d, 0755);
    return 0;
}
C
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" log)" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! ${CC} -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -g -iquote "${S}" \
        -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" -lz -lm -lpthread > "${WORK}/cc.log" 2>&1; then
    echo "  FAIL: harness does not build"; sed 's/^/      /' "${WORK}/cc.log" | head -8
    echo "Results: 0 passed, 1 failed"; exit 1
fi

count() { local n; n="$(grep -c "$1" "$2" 2>/dev/null)"; echo "${n:-0}"; }
check() {  # check NAME COND WHY
    if [ "$2" = 1 ]; then echo "  PASS: $1"; PASS=$((PASS+1));
    else echo "  FAIL: $1 -- $3"; FAIL=$((FAIL+1)); fi
}

d="${WORK}/rw"; mkdir -p "${d}"
"${WORK}/h" "${d}" rw 2> "${d}/stderr"; rc=$?
check "harness exits 0 (rw)" "$([ ${rc} = 0 ] && echo 1)" "exit ${rc}"
n="$(count post-main "${d}/app.log")"; o="$(count post-main "${d}/app.log.1")"
check "set-output: post-rotation line in the new file" "$([ "${n}" = 1 ] && [ "${o}" = 0 ] && echo 1)" \
      "new=${n} rotated=${o}"
n="$(count post-sec "${d}/sec.log")"; o="$(count post-sec "${d}/sec.log.1")"
check "security: post-rotation line in the new file" "$([ "${n}" = 1 ] && [ "${o}" = 0 ] && echo 1)" \
      "new=${n} rotated=${o}"
p="$(count pre-main "${d}/app.log.1")"; q="$(count pre-sec "${d}/sec.log.1")"
check "pre-rotation lines stay in the rotated files" "$([ "${p}" = 1 ] && [ "${q}" = 1 ] && echo 1)" \
      "main=${p} sec=${q}"

if [ "$(id -u)" = 0 ]; then
    echo "  SKIP: unreopenable path -- running as root, chmod 555 does not stop writes"
else
    d="${WORK}/ro"; mkdir -p "${d}"
    "${WORK}/h" "${d}" ro 2> "${d}/stderr"; rc=$?
    check "harness exits 0 (unreopenable)" "$([ ${rc} = 0 ] && echo 1)" "exit ${rc}"
    o="$(count post-main "${d}/app.log.1")"; s="$(count post-sec "${d}/sec.log.1")"
    check "unreopenable path: lines kept in the old file, not lost" \
          "$([ "${o}" = 1 ] && [ "${s}" = 1 ] && echo 1)" "main=${o} sec=${s}"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" = 0 ]
