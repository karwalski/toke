#!/bin/sh
# check_downstream.sh — do toke's two downstream consumers still build against
# THIS toke tree?  (story 121.82; `make check-downstream`)
#
# Usage: scripts/check_downstream.sh <toke-dir> <workspace-dir>
#
# ooke and toke-website both link toke's stdlib C files, taking the list from
# the candidate compiler's `tkc --emit-deps` (src/stdlib_deps.c).  A toke change
# that adds, moves or splits a stdlib C file, or edits the manifest or a .tki,
# can break their link without breaking anything in this repo: on 2026-10-01
# os_glue.c broke ooke's link (121.70/121.76) and toml->file broke the site
# (C661).  This runs both against <toke-dir>, whose compiler must be built:
#
#   ooke     `make check-toke-candidate TOKE_CANDIDATE=<toke-dir>` (ooke 121.76:
#            builds the binary, links + runs the unit tests, `ooke compile`s an app)
#   website  `make website` + `make check-routes` with TKC/TOKE_REPO = <toke-dir>
#            and OOKE = the ooke binary the ooke stage built against <toke-dir>
#            (132.113: the website's own OOKE default is whatever pre-built
#            ~/tk/toke-ooke/ooke-toke happens to exist, so /docs was rendered
#            by an ooke that had never seen the candidate).  The ooke worktree
#            lives until the trap at exit, after the website stage.
#
# Each runs in a temporary detached worktree of that repo's `main` (override:
# OOKE_REF=, WEBSITE_REF=), so the sibling checkouts -- whatever branch they are
# on, whatever they have uncommitted -- are neither tested nor touched.
# A sibling that is not checked out under <workspace-dir> is SKIPPED with a
# message, not failed; a sibling that is present but fails is a failure.
set -u

toke=$(cd "${1:?usage: check_downstream.sh <toke-dir> <workspace-dir>}" && pwd) || exit 2
ws=${2:?usage: check_downstream.sh <toke-dir> <workspace-dir>}
[ -x "$toke/toke" ] || { echo "check-downstream: $toke/toke is not built" >&2; exit 2; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/toke-downstream.XXXXXX")
wts=""
cleanup() {
  for w in $wts; do git -C "${w%%|*}" worktree remove --force "${w#*|}" >/dev/null 2>&1 || true; done
  rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

fails=""; skips=""; oks=""; ooke_bin=""
# worktree <repo> <ref> <name> -> prints the worktree path, or returns 1.
# It runs in a $(...) subshell, so the caller records it in $wts for cleanup.
worktree() {
  sha=$(git -C "$1" rev-parse --verify -q "$2^{commit}") || return 1
  git -C "$1" worktree add --detach "$tmp/$3" "$sha" >/dev/null 2>&1 || return 1
  echo "$tmp/$3"
}

echo "=== check-downstream: toke $(git -C "$toke" rev-parse --short HEAD 2>/dev/null || echo '?') at $toke"
echo "    siblings from TOKE_WORKSPACE=$ws"

# ── ooke ────────────────────────────────────────────────────────────────────
ooke=$ws/toke-ooke; ref=${OOKE_REF:-main}
if ! git -C "$ooke" rev-parse --git-dir >/dev/null 2>&1; then
  echo "--- ooke: SKIP (no checkout at $ooke)"; skips="$skips ooke"
elif ! wt=$(worktree "$ooke" "$ref" ooke); then
  echo "--- ooke: FAIL (cannot check out '$ref' of $ooke)" >&2; fails="$fails ooke"
elif wts="$wts $ooke|$wt"; ! grep -q '^check-toke-candidate:' "$wt/Makefile"; then
  echo "--- ooke: FAIL ($ref predates check-toke-candidate, ooke ff4b50a)" >&2; fails="$fails ooke"
else
  echo "--- ooke $(git -C "$wt" rev-parse --short HEAD) ($ref): make check-toke-candidate"
  if make -C "$wt" --no-print-directory check-toke-candidate TOKE_CANDIDATE="$toke"; then
    # check-toke-candidate builds in a worktree of its own and removes it, so
    # build the binary again here, in this worktree, for the website stage.
    echo "--- ooke: building $wt/ooke-toke against $toke for the website stage"
    if make -s -C "$wt" --no-print-directory TOKE_ROOT="$toke" ooke-toke >"$tmp/ooke-build.log" 2>&1 &&
       [ -x "$wt/ooke-toke" ]; then
      ooke_bin=$wt/ooke-toke; oks="$oks ooke"
    else
      tail -20 "$tmp/ooke-build.log" >&2
      echo "--- ooke: FAIL (ooke-toke did not build in $wt)" >&2; fails="$fails ooke"
    fi
  else fails="$fails ooke"; fi
fi

# ── toke-website ────────────────────────────────────────────────────────────
site=$ws/toke-website; ref=${WEBSITE_REF:-main}
if ! git -C "$site" rev-parse --git-dir >/dev/null 2>&1; then
  echo "--- website: SKIP (no checkout at $site)"; skips="$skips website"
elif ! wt=$(worktree "$site" "$ref" website); then
  echo "--- website: FAIL (cannot check out '$ref' of $site)" >&2; fails="$fails website"
else
  wts="$wts $site|$wt"
  echo "--- website $(git -C "$wt" rev-parse --short HEAD) ($ref): make website check-routes"
  if [ -n "$ooke_bin" ]; then
    echo "    ooke: $ooke_bin (built against $toke)"
    set -- OOKE="$ooke_bin"
  else
    echo "    ooke: the website's default OOKE -- NOT built against $toke (ooke stage did not pass)"
    set --
  fi
  if make -C "$wt" --no-print-directory website TKC="$toke/toke" TOKE_REPO="$toke" "$@" &&
     make -C "$wt" --no-print-directory check-routes TKC="$toke/toke" TOKE_REPO="$toke" "$@"; then
    oks="$oks website"; else fails="$fails website"; fi
fi

echo "=== check-downstream: ok:${oks:- none}  skipped:${skips:- none}  failed:${fails:- none}"
[ -z "$fails" ]
