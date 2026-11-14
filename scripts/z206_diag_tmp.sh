#!/usr/bin/env bash
# TEMPORARY Z206 diagnostics (removed before handoff)
set -x
uname -a; ldd --version | head -1; cc --version | head -1; clang --version | head -2
command -v clang ld ld.lld lld llvm-objdump objdump strace perf || true
readlink -f "$(command -v clang)"
clang -print-prog-name=ld; clang -print-prog-name=llvm-objdump; clang -print-search-dirs | head -3
ls -la /usr/x86_64-linux-gnu/bin /usr/aarch64-linux-gnu/bin 2>&1 | head
grep TKC_CLANG_PATH build_info.h src/build_info.h 2>/dev/null
W=$(mktemp -d); cd "$W"
printf 'm=q;f=main():i64{<7};\n' > p.tk
env PATH= "$GITHUB_WORKSPACE/toke" --out p p.tk; echo "rc=$?"
env PATH= "$(readlink -f "$(command -v clang)")" -v -x c /dev/null -o x 2>&1 | tail -5
env PATH= /usr/bin/clang -### -x c /dev/null -o x 2>&1 | tail -3
# C1611
cat > j.tk <<'TK'
m=j;
i=log:std.log;
f=main():i64{
  let a=log.info("i"; @());
  if(a){<9};
  <0
};
TK
"$GITHUB_WORKSPACE/toke" --allow-all --out j j.tk; echo "build rc=$?"
sudo apt-get install -y strace >/dev/null 2>&1
strace -f -o st.txt env TK_STR_VERIFY=1 ./j 2>&-; echo "j rc=$?"
grep -nE 'open|dup|fcntl|close\(|write\(2|socket|pipe|eventfd|memfd' st.txt | grep -v ENOENT | tail -40
strace -f -o st2.txt ./j 2>&-; echo "j (no verify) rc=$?"
grep -nE 'open|write\(2' st2.txt | grep -v ENOENT | tail -15
# C546
dpkg -l tzdata | tail -1
ls -la /usr/share/zoneinfo/EST5EDT /usr/share/zoneinfo/posixrules /usr/share/zoneinfo/America/New_York 2>&1
tail -c 40 /usr/share/zoneinfo/EST5EDT | od -c | head
head -c 60 /usr/share/zoneinfo/EST5EDT | od -A d -t u1 | head -5
for t in 1730606400 1730608200 1730610000 1730610001 1730611800 1730613599 1730613600 1730617200; do
  echo "$t EST5EDT=$(TZ=EST5EDT date -d @$t '+%F %T %z') NY=$(TZ=America/New_York date -d @$t '+%F %T %z') posix=$(TZ='EST5EDT,M3.2.0,M11.1.0' date -d @$t '+%F %T %z')"
done
for t in 1710053999 1710054000 1699164000 1699167599 1699167600; do
  echo "$t EST5EDT=$(TZ=EST5EDT date -d @$t '+%F %T %z') NY=$(TZ=America/New_York date -d @$t '+%F %T %z')"
done
# perf
sudo apt-get install -y "linux-tools-$(uname -r)" linux-tools-common >/dev/null 2>&1 || sudo apt-get install -y linux-tools-generic >/dev/null 2>&1
command -v perf; cat /proc/sys/kernel/perf_event_paranoid
sudo sysctl -w kernel.perf_event_paranoid=1
perf stat -x, -e instructions:u true 2>&1 | tail -3
ls /usr/lib/linux-tools*/ 2>&1 | head
exit 0
