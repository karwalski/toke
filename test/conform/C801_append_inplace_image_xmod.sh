#!/usr/bin/env bash
# C801_append_inplace_image_xmod.sh — a linear [byte] accumulator keeps its
# in-place append when it is later passed to std.image, or to a user function
# of ANOTHER module of the same `tkc -o` build that does not retain it
# (127.306).
#
# Before 127.306 `o=o.append(v)` lowered to the copying tk_array_append_w
# (O(N^2) time and, since discarded copies are never freed, O(N^2) memory)
# whenever the accumulator was later handed to image.fromraw (std.image was
# not vouched for, 127.25) or to `lib.n(o)` in another module (127.108 only
# reads callee bodies of the current module).  Now:
#   * std.image decode / fromraw / tiffpages / tiffdecode / convolve are
#     vouched for method by method (image_glue.c copies the array out through
#     tk_bytes_unpack, or reads the kernel during the call, and keeps nothing);
#   * during a multi-module build each module records which parameters of its
#     functions pass la_param_nonretaining, and later modules trust that
#     record for calls into it.
#
# Checks:
#   1. IR: in-place appends (and no copying ones) in the builder for
#      image.fromraw / decode / tiffpages and an [f64] kernel to convolve.
#   2. IR of the multi-module build (captured through a TKC_CC wrapper that
#      copies the .ll files clang is given): lib.n and the transitive lib.via
#      keep the in-place path; lib.keep (returns its param), lib.nest (stores
#      it in an array it returns) and lib.viakeep (returns it through keep)
#      keep the COPYING path.
#   3. Aliasing correctness: after each such call the caller mutates its
#      accumulator; values the callee returned (image data, kept arrays) must
#      not see the mutation, and the accumulator must hold its own values.
#   4. Shape (Darwin only, ru_maxrss in bytes): peak RSS at N=6000 vs N=2000
#      appends.  In place the ratio is ~1; the copying path is ~9 (and stays
#      under ~200 MB at N=6000, so a regression cannot exhaust the host).
#
# Story: 127.306

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc not found at ${TKC}"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c801.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

# count <ll> <fn-symbol>  ->  "<inplace> <copying>" appends in that function
count() {
    awk -v fn="@$2(" 'index($0, "define ") == 1 && index($0, fn) {f=1}
        f && /call i64 @tk_array_append_inplace_w/ {i++}
        f && /call i64 @tk_array_append_w/ {c++}
        f && /^}/ {f=0}
        END {printf "%d %d", i, c}' "$1"
}

# ---- 1. std.image callees (single module, --emit-llvm) -------------------
img_case() {   # name, accumulator type, element expr, tail expression
    local name="$1" ty="$2" el="$3" tail="$4" d="${WORK}/img_$1"
    mkdir -p "$d"
    cat > "$d/p.tk" <<EOF
m=main;
i=io:std.io;
i=image:std.image;
f=build(n:i64):i64{
  let o:${ty}=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(${el})};
  ${tail}
};
f=main():i64{
  io.println("\(build(16) as \$str)");
  <0
};
EOF
    if ! "${TKC}" --emit-llvm "$d/p.tk" -o "$d/p.ll" > "$d/log" 2>&1; then
        bad "image.${name}: --emit-llvm failed: $(head -c 300 "$d/log")"; return
    fi
    local r; r="$(count "$d/p.ll" main_build)"
    if [ "$r" = "1 0" ]; then ok "image.${name}: builder append stays in place (inplace/copying = ${r})"
    else bad "image.${name}: builder appends inplace/copying = ${r}, want 1 0"; fi
}
img_case fromraw '@byte' '(i%256) as byte' \
    '<image.fromraw(o;4 as u32;4 as u32;1 as u8).width as i64'
img_case decode '@byte' '(i%256) as byte' \
    '<mt image.decode(o){$ok:b b.width as i64;$err:e 0-1}'
img_case tiffpages '@byte' '(i%256) as byte' \
    '<mt image.tiffpages(o){$ok:p p as i64;$err:e 0-1}'
img_case tiffdecode '@byte' '(i%256) as byte' \
    '<mt image.tiffdecode(o;0 as u32){$ok:b b.width as i64;$err:e 0-1}'
img_case convolve '@f64' '1.0' \
    'let im=image.fromraw(@(1 as byte;2 as byte;3 as byte;4 as byte);2 as u32;2 as u32;1 as u8);
  <mt image.convolve(im;o;3 as u32;9.0;0.0){$ok:b b.width as i64;$err:e 0-1}'

# ---- 2/3. cross-module callees in ONE multi-module build -----------------
REALCC="$(command -v clang || true)"
if [ -z "${REALCC}" ]; then
    bad "clang not on PATH (needed for the TKC_CC capture wrapper)"
else
    X="${WORK}/xmod"; mkdir -p "$X/ll"
    cat > "$X/cc.sh" <<'EOF'
#!/bin/sh
for a in "$@"; do case "$a" in *.ll) cp "$a" "$LLDIR/" ;; esac; done
exec "$REALCC" "$@"
EOF
    chmod +x "$X/cc.sh"
    cat > "$X/lib.tk" <<'EOF'
m=lib;
f=n(a:@byte):i64{<a.len};
f=keep(a:@byte):@byte{<a};
f=nest(a:@byte):@@byte{<@(a)};
f=via(a:@byte):i64{<n(a)};
f=viakeep(a:@byte):@byte{let r=keep(a);<r};
f=grow(a:@byte):i64{let b=a.append(1 as byte);let c=b.set(0;55 as byte);<c.len};
EOF
    cat > "$X/main.tk" <<'EOF'
m=main;
i=io:std.io;
i=image:std.image;
i=lib:lib;
f=img():i64{
  let o:@byte=mut.@();
  lp(let i=0;i<4;i=i+1){o=o.append((i+10) as byte)};
  let im=image.fromraw(o;2 as u32;2 as u32;1 as u8);
  o=o.set(0;99 as byte);
  o=o.append(7 as byte);
  <(im.data.get(0) as i64)*1000+(o.get(0) as i64)*10+o.len
};
f=xkeep():i64{
  let o:@byte=mut.@();
  lp(let i=0;i<4;i=i+1){o=o.append((i+10) as byte)};
  let k=lib.keep(o);
  o=o.set(0;99 as byte);
  o=o.append(7 as byte);
  <(k.get(0) as i64)*1000+k.len*10+o.len
};
f=xnest():i64{
  let o:@byte=mut.@();
  lp(let i=0;i<4;i=i+1){o=o.append((i+10) as byte)};
  let k=lib.nest(o);
  o=o.set(0;99 as byte);
  <(k.get(0).get(0) as i64)*10+k.get(0).len
};
f=xviakeep():i64{
  let o:@byte=mut.@();
  lp(let i=0;i<4;i=i+1){o=o.append((i+10) as byte)};
  let k=lib.viakeep(o);
  o=o.set(0;99 as byte);
  <(k.get(0) as i64)*10+k.len
};
f=xn():i64{
  let o:@byte=mut.@();
  lp(let i=0;i<4;i=i+1){o=o.append((i+10) as byte)};
  let a=lib.n(o);
  o=o.append(7 as byte);
  let b=lib.via(o);
  o=o.set(0;99 as byte);
  <a*100+b*10+(o.get(0) as i64)
};
f=xgrow():i64{
  let o:@byte=mut.@();
  lp(let i=0;i<4;i=i+1){o=o.append((i+10) as byte)};
  let g=lib.grow(o);
  o=o.append(7 as byte);
  <g*1000+o.len*100+(o.get(0) as i64)
};
f=main():i64{
  io.println("img=\(img() as $str)");
  io.println("xkeep=\(xkeep() as $str)");
  io.println("xnest=\(xnest() as $str)");
  io.println("xviakeep=\(xviakeep() as $str)");
  io.println("xn=\(xn() as $str)");
  io.println("xgrow=\(xgrow() as $str)");
  <0
};
EOF
    if ! ( cd "$X" && LLDIR="$X/ll" REALCC="${REALCC}" TKC_CC="$X/cc.sh" \
             "${TKC}" -o p lib.tk main.tk > build.log 2>&1 ); then
        bad "multi-module build failed: $(head -c 300 "$X/build.log")"
    else
        MLL=""
        for f in "$X"/ll/*.ll; do grep -q '^define .*@main_xn(' "$f" && MLL="$f"; done
        if [ -z "${MLL}" ]; then
            bad "could not capture main.tk's IR through TKC_CC"
        else
            # every append in the function (the loop, and any after the call)
            for spec in xn:"2 0" xgrow:"2 0" img:"2 0" \
                        xkeep:"0 2" xnest:"0 1" xviakeep:"0 1"; do
                fn="${spec%%:*}"; want="${spec#*:}"
                got="$(count "${MLL}" "main_${fn}")"
                if [ "$got" = "$want" ]; then ok "IR main.${fn}: appends inplace/copying = ${got}"
                else bad "IR main.${fn}: appends inplace/copying = ${got}, want ${want}"; fi
            done
        fi
        out="$("$X/p" 2>&1)"
        # img: image data keeps 10; o is 99.. with 5 elements
        # xkeep/xnest/xviakeep: the kept array still starts 10, len 4
        # xn: n=4, via=5, o[0]=99;  xgrow: grow's append+set (len 5, [0]=55)
        # must not reach o, which is len 5 and still starts 10
        want="img=10995
xkeep=10045
xnest=104
xviakeep=104
xn=549
xgrow=5510"
        if [ "$out" = "$want" ]; then ok "aliasing correctness (6 cases) prints the expected values"
        else bad "aliasing correctness: got
${out}
want
${want}"; fi
    fi
fi

# ---- 4. memory shape (Darwin) --------------------------------------------
if [ "$(uname -s)" = "Darwin" ] && [ -x /usr/bin/time ]; then
    S="${WORK}/shape"; mkdir -p "$S"
    printf 'm=lib;\nf=n(a:@byte):i64{<a.len};\n' > "$S/lib.tk"
    rss_of() {   # kind N -> peak RSS bytes
        local kind="$1" n="$2" tail
        case "$kind" in
            xmod) tail='<lib.n(o)' ;;
            image) tail='<image.fromraw(o;100 as u32;100 as u32;1 as u8).width as i64' ;;
        esac
        cat > "$S/m.tk" <<EOF
m=main;
i=io:std.io;
i=image:std.image;
i=lib:lib;
f=build(n:i64):i64{
  let o:@byte=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append((i%256) as byte)};
  ${tail}
};
f=main():i64{
  io.println("\(build(${n}) as \$str)");
  <0
};
EOF
        ( cd "$S" && "${TKC}" -o b lib.tk m.tk > build.log 2>&1 ) || { echo 0; return; }
        { /usr/bin/time -l "$S/b" > /dev/null; } 2>&1 | awk '/maximum resident/{print $1}'
    }
    for kind in xmod image; do
        a="$(rss_of "$kind" 2000)"; b="$(rss_of "$kind" 6000)"
        if [ -z "$a" ] || [ -z "$b" ] || [ "$a" -le 0 ] 2>/dev/null; then
            bad "${kind} shape: could not measure (rss 2000=${a} 6000=${b})"
        elif [ $((b * 10)) -lt $((a * 25)) ]; then
            ok "${kind} shape: peak RSS 6000/2000 appends = ${b}/${a} (< 2.5x; copying is ~9x)"
        else
            bad "${kind} shape: peak RSS 6000/2000 appends = ${b}/${a} (>= 2.5x: quadratic)"
        fi
    done
else
    echo "SKIP shape: ru_maxrss units are host-specific (Darwin only)"
fi

echo "C801: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
