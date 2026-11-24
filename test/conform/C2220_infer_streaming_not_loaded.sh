#!/usr/bin/env bash
# C2220_infer_streaming_not_loaded.sh — infer.loadstreaming refuses with an
# explicit "not implemented" error instead of handing out a handle that is
# never loaded (story 136.78).
#
# THE DEFECT.  In a llama.cpp build (TK_HAVE_LLAMACPP), tk_infer_load_streaming
#   checked the directory, built the shard list, allocated the prefetch ring and
#   a TkModelHandle, started the prefetch thread and returned SUCCESS.  The
#   layer loop that would load the shards into a llama_model/llama_context is
#   a TODO, so model and ctx stayed NULL: tk_infer_is_loaded said 0 for the
#   handle, and every generate/embed on it failed, after a load that had
#   reported success.  A silent 0, not an error.
#
# THE FIX (the row's documented alternative; real-model work is behind the
#   compute hold).  After the checks that can already be made (an empty
#   model_dir, a directory with no layer_NNN.gguf shard: code -1), the call
#   returns is_err=1, code=-2 ("not implemented", infer.h) and a message that
#   says so.  No handle, no prefetch thread.  The machinery is kept behind
#   TK_INFER_STREAM_LAYER_LOOP, to be defined with the loop itself.  The stub
#   build (no llama.cpp, the default) already returned -2 and is unchanged;
#   so is the toke-level glue, which takes the $err arm.
#
# WHAT IS ASSERTED.
#   Part 1: src/stdlib/infer_stream.c compiled WITH -DTK_HAVE_LLAMACPP (the
#     file names no llama.cpp symbol, so it builds without the library) and a
#     C driver: a directory holding two shards gives is_err=1, code=-2, a NULL
#     handle and a message containing "not implemented"; an empty directory
#     and "" give code -1; stream_cleanup / stream_throughput see no
#     registered stream.  Built with -fsanitize=address,undefined: the error
#     path frees the stream state it allocated (LeakSanitizer, where the
#     platform supports it).
#   Part 2: the same file in the default (stub) build still returns code -2
#     and a NULL handle.
#   Part 3: a toke program calling infer.loadstreaming takes the $err arm.
#
# Negative control (measured 2026-11-24): against the base 42fafa61 Part 1
#   fails (is_err=0, code=0, a non-NULL handle, stream_cleanup finds the
#   registered stream); Parts 2 and 3 pass there too.
#
# Story: 136.78
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C2220_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2220_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C2220: infer.loadstreaming returns an explicit not-implemented error (136.78)"
echo "---------------------------------------------------------------------------"
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

mkdir -p model empty
: > model/layer_000.gguf; : > model/layer_001.gguf

cat > drv.c <<'EOF'
#include "infer_stream.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    TkStreamOpts o; memset(&o, 0, sizeof o);
    o.ram_ceiling_gb = 1.0f; o.prefetch_layers = 1;
    int bad = 0;
    TkModelHandleResult r = tk_infer_load_streaming(argv[1], o);
    printf("shards: is_err=%d code=%d handle=%s msg=%s\n", r.is_err, (int)r.err.code,
           r.handle ? "set" : "null", r.err.msg ? r.err.msg : "(null)");
    if (!(r.is_err == 1 && r.err.code == -2 && r.handle == NULL && r.err.msg &&
          strstr(r.err.msg, "not implemented"))) bad |= 1;
    if (r.handle) {   /* base: the handle is registered with a live thread */
        printf("cleanup(handle)=%d\n", tk_infer_stream_cleanup(r.handle));
        bad |= 1;
    }
    if (argc > 2) {
        TkModelHandleResult e = tk_infer_load_streaming(argv[2], o);
        printf("empty: is_err=%d code=%d handle=%s\n", e.is_err, (int)e.err.code,
               e.handle ? "set" : "null");
        if (!(e.is_err == 1 && e.err.code == -1 && e.handle == NULL)) bad |= 2;
    }
    TkModelHandleResult z = tk_infer_load_streaming("", o);
    printf("blank: is_err=%d code=%d\n", z.is_err, (int)z.err.code);
    if (!(z.is_err == 1 && z.err.code == -1 && z.handle == NULL)) bad |= 4;
    TkStreamThroughput t = tk_infer_stream_throughput(NULL);
    if (tk_infer_stream_cleanup(NULL) != 0 || t.bytes_loaded != 0) bad |= 8;
    printf("bad=%d\n", bad);
    return bad ? 1 : 0;
}
EOF

# ── Part 1: the llama.cpp build of the streaming loader ──────────────────────
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
if ${CC} -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -g ${SAN} -DTK_HAVE_LLAMACPP \
        -iquote "${S}" -o drv_llama drv.c "${S}/infer_stream.c" -lpthread 2> cc1.err; then
    case "$(uname -s)" in Linux) export ASAN_OPTIONS=detect_leaks=1 ;; *) export ASAN_OPTIONS=detect_leaks=0 ;; esac
    if ./drv_llama model empty > out1.txt 2> err1.txt; then
        ok "llama build: shards -> code -2, no handle; empty dir and \"\" -> code -1"
    else
        bad "llama build: $(tr '\n' ' ' < out1.txt)"
    fi
    if grep -q 'ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:' err1.txt; then
        bad "llama build: sanitizer report"; sed -n 1,20p err1.txt
    else
        ok "llama build: no sanitizer report"
    fi
else
    bad "llama build: infer_stream.c with -DTK_HAVE_LLAMACPP did not compile"; sed -n 1,20p cc1.err
fi

# ── Part 2: the default (stub) build ─────────────────────────────────────────
if ${CC} -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -g \
        -iquote "${S}" -o drv_stub drv.c "${S}/infer_stream.c" -lpthread 2> cc2.err; then
    # The stub's message names llama.cpp rather than "not implemented", so
    # the driver's exit status is not used here: the contract is the code
    # and the NULL handle.
    ./drv_stub model > out2.txt 2>&1
    if grep -q 'shards: is_err=1 code=-2 handle=null' out2.txt; then
        ok "stub build: code -2, no handle"
    else
        bad "stub build: $(tr '\n' ' ' < out2.txt)"
    fi
else
    bad "stub build did not compile"; sed -n 1,20p cc2.err
fi

# ── Part 3: the toke surface ─────────────────────────────────────────────────
cat > prog.tk <<'EOF'
m=prog;
i=infer:std.infer;
i=io:std.io;
f=main():i64{
  let o=$streamopts{ramceilinggb:1.0 as f32;prefetchlayers:1 as i32;requiresnvme:false};
  let r=infer.loadstreaming("model";o);
  let s=mt r {$ok:h "ok";$err:e "err"};
  (io.println(s));
  <0
};
EOF
if "${TKC}" prog.tk -o prog > tkc3.err 2>&1; then
    out3="$(./prog 2>&1)"
    if [ "${out3}" = "err" ]; then ok "toke: infer.loadstreaming takes the \$err arm"
    else bad "toke: expected 'err', got '${out3}'"; fi
else
    bad "toke program did not compile"; sed -n 1,20p tkc3.err
fi

echo ""
echo "C2220: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
