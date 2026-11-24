/*
 * test_cap_os.c — unit test for the OS-level enforcement layer behind the
 * capability broker (story 138.44, src/stdlib/cap_os.h).
 *
 * Two halves:
 *   1. platform-neutral: the macOS SBPL profile text and the Linux Landlock
 *      rights a plan produces, on any host (the Linux rules are therefore
 *      tested on macOS too, without a Linux kernel);
 *   2. the kernel: a forked child applies a plan with tk_os_apply and then
 *      opens files directly (fopen, no broker in the way).  The scoped file
 *      opens, the file outside the scope does not.  On a host with no OS
 *      layer (or a Linux kernel without Landlock) the child reports CAP004
 *      through the warn callback, and the kernel half is a named skip.
 *
 * Compile (the Makefile's test-stdlib-capabilities target runs it):
 *   cc -std=c99 -D_GNU_SOURCE -iquote src/stdlib test/stdlib/test_cap_os.c -o /tmp/tco
 */
#include "cap_os.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define ASSERT(cond, msg) do { \
    if (cond) printf("pass: %s\n", (msg)); \
    else { printf("FAIL: %s\n", (msg)); failures++; } } while (0)

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

static char *render_sbpl(const TkOsPlan *p) {
    TkOsBuf b = {0};
    tk_os_sbpl(p, &b);
    return b.oom ? NULL : b.p;
}

static char *render_ll(const TkOsPlan *p, int abi) {
    TkOsBuf b = {0};
    tk_ll_describe(p, abi, &b);
    return b.oom ? NULL : b.p;
}

static void test_plan_text(void) {
    TkOsPlan p;
    memset(&p, 0, sizeof p);

    /* Nothing restricted: allow default and nothing else. */
    char *s = render_sbpl(&p);
    ASSERT(s && !strcmp(s, "(version 1)\n(allow default)\n"), "empty plan: profile is allow default only");
    free(s);

    p.read.on = 1;
    ASSERT(tk_os_add_path(&p.read, "/srv/www"), "add a read scope");
    ASSERT(tk_os_add_path(&p.read, "/srv/www"), "a duplicate read scope is accepted");
    ASSERT(p.read.n == 1, "... and kept once");
    ASSERT(!tk_os_add_path(&p.read, "relative/path"), "a relative path is refused");
    ASSERT(!tk_os_add_path(&p.read, "/a\nb"), "a path with a control character is refused");
    ASSERT(tk_os_add_path(&p.read, "/q\"uo\\te"), "a path with a quote and a backslash is accepted");
    p.cwd = strdup("/work");
    p.write.on = 1;
    ASSERT(tk_os_add_path(&p.write, "/srv/cache"), "add a write scope");
    p.exec.on = 1;
    ASSERT(tk_os_add_path(&p.exec, "/usr/bin/git"), "add a command");
    p.bind.on = 1;
    ASSERT(tk_os_add_port(&p.bind, 8080, 1), "add a loopback listen port");
    p.connect.on = 1;
    ASSERT(tk_os_add_port(&p.connect, 443, 0), "add a connect port");
    ASSERT(tk_os_add_port(&p.connect, -1, 1), "add a loopback any-port connect");

    s = render_sbpl(&p);
    ASSERT(has(s, "(deny file-read-data)\n(allow file-read-data\n  (subpath \"/usr\")"),
           "sbpl: read is denied, then the system baseline admitted");
    ASSERT(has(s, "(subpath \"/srv/www\")"), "sbpl: the read scope is a subpath");
    ASSERT(has(s, "(subpath \"/q\\\"uo\\\\te\")"), "sbpl: a quote and a backslash are escaped");
    ASSERT(has(s, "(allow file-read-data (literal \"/work\"))"), "sbpl: the working directory itself (getcwd)");
    ASSERT(!has(s, "(subpath \"/work\")"), "sbpl: ... not beneath it");
    ASSERT(has(s, "(deny file-write*)\n(allow file-write*\n  (literal \"/dev/null\")"),
           "sbpl: write is denied, then /dev/null admitted");
    ASSERT(has(s, "(subpath \"/srv/cache\"))"), "sbpl: the write scope is a subpath");
    ASSERT(has(s, "(deny process-exec*)\n(allow process-exec* (literal \"/usr/bin/git\"))"),
           "sbpl: exec is denied but for the granted command");
    ASSERT(has(s, "(deny network-bind (local ip \"*:*\"))\n(allow network-bind (local ip \"localhost:8080\"))"),
           "sbpl: bind narrows to the loopback port");
    ASSERT(has(s, "(allow network-outbound (remote ip \"*:443\"))"), "sbpl: connect narrows to the port");
    ASSERT(has(s, "(allow network-outbound (remote ip \"localhost:*\"))"), "sbpl: loopback, any port");
    ASSERT(!has(s, "\"/home") && !has(s, "\"/Users") && !has(s, "\"/tmp") && !has(s, "\"/private/tmp") && !has(s, "\"/private/var/folders"), "sbpl: no home or temporary directory in the baseline");
    free(s);

    /* Linux: rights by ABI. */
    ASSERT(tk_ll_write_rights(1) == 0x1FF2ull, "landlock abi 1 write rights");
    ASSERT(tk_ll_write_rights(2) == (0x1FF2ull | TK_LL_REFER), "landlock abi 2 adds REFER");
    ASSERT(tk_ll_write_rights(3) == (0x1FF2ull | TK_LL_REFER | TK_LL_TRUNCATE), "landlock abi 3 adds TRUNCATE");
    ASSERT(tk_ll_handled_fs(&p, 3) == (tk_ll_read_rights() | tk_ll_write_rights(3) | TK_LL_EXECUTE),
           "landlock handles read, write and execute when all three are restricted");
    ASSERT(tk_ll_handled_net(&p, 3) == 0, "landlock abi 3: no TCP rules");
    ASSERT(tk_ll_handled_net(&p, 4) == TK_LL_NET_BIND,
           "landlock abi 4: bind handled; a connect scope with any port leaves connect to the broker");
    s = render_ll(&p, 4);
    ASSERT(has(s, "read scope /srv/www\n"), "linux: read scope rule");
    ASSERT(has(s, "write scope /srv/cache\n"), "linux: write scope rule");
    ASSERT(has(s, "execute scope /usr/bin/git\n"), "linux: execute rule");
    ASSERT(has(s, "execute base /lib64\n"), "linux: the dynamic loader can be executed");
    ASSERT(has(s, "bind port 8080\n") && !has(s, "connect port"), "linux: bind port rule, no connect rule");
    ASSERT(!has(s, "seccomp"), "linux: no socket filter while net is granted");
    free(s);

    TkOsPlan q;
    memset(&q, 0, sizeof q);
    q.bind.on = q.connect.on = 1;
    q.net_none = 1;
    ASSERT(tk_ll_handled_net(&q, 4) == (TK_LL_NET_BIND | TK_LL_NET_CONNECT),
           "net not granted: both TCP directions handled, no port admitted");
    ASSERT(tk_ll_handled_fs(&q, 4) == 0, "net only: no fs rights handled");
    s = render_ll(&q, 4);
    ASSERT(has(s, "seccomp socket AF_INET AF_INET6"), "net not granted: the seccomp socket filter");
    free(s);
    s = render_sbpl(&q);
    ASSERT(has(s, "(deny network-bind (local ip \"*:*\"))\n(deny network-outbound (remote ip \"*:*\"))\n"),
           "sbpl: net not granted denies every bind and IP connect");
    ASSERT(!has(s, "file-"), "sbpl: net only touches no file rule");
    free(s);
    tk_os_plan_free(&q);
    tk_os_plan_free(&p);
}

/* ── The seccomp filter, run on a tiny classic-BPF interpreter ───── */

static uint32_t bpf_run(const TkBpf *f, int n, uint32_t arch, uint32_t nr, uint64_t arg0) {
    unsigned char d[64];
    memset(d, 0, sizeof d);
    int32_t snr = (int32_t)nr;
    memcpy(d + TK_SC_OFF_NR, &snr, 4);
    memcpy(d + TK_SC_OFF_ARCH, &arch, 4);
    memcpy(d + TK_SC_OFF_ARG0, &arg0, 8);
    uint32_t a = 0;
    for (int pc = 0; pc < n; pc++) {
        const TkBpf *i = &f[pc];
        switch (i->code) {
        case TK_BPF_LD_W_ABS: if (i->k + 4 > sizeof d) return 0xDEADu; memcpy(&a, d + i->k, 4); break;
        case TK_BPF_JEQ_K:    pc += (a == i->k) ? i->jt : i->jf; break;
        case TK_BPF_JGE_K:    pc += (a >= i->k) ? i->jt : i->jf; break;
        case TK_BPF_RET_K:    return i->k;
        default:              return 0xBADu;
        }
    }
    return 0xFA11u;   /* ran off the end: the kernel would refuse the program */
}

static void test_seccomp_filter(void) {
    const uint32_t deny = TK_SC_RET_ERRNO | EACCES, allow = TK_SC_RET_ALLOW, kill = TK_SC_RET_KILL_PROCESS;
    const uint32_t X86_64 = 0xC000003Eu, I386 = 0x40000003u, AARCH64 = 0xC00000B7u, ARM = 0x40000028u;
    TkBpf f[TK_SC_FILTER_LEN];
    int n = tk_sc_no_net_filter(&tk_sc_x86_64, f);
    int inrange = n == TK_SC_FILTER_LEN && f[n - 1].code == TK_BPF_RET_K;
    for (int i = 0; i < n; i++)
        if ((f[i].code == TK_BPF_JEQ_K || f[i].code == TK_BPF_JGE_K) &&
            (i + 1 + f[i].jt >= n || i + 1 + f[i].jf >= n)) inrange = 0;
    ASSERT(inrange, "seccomp: every jump lands inside the program, which ends in a return");
    ASSERT(bpf_run(f, n, X86_64, 41, 2) == deny, "seccomp x86_64: socket(AF_INET) refused");
    ASSERT(bpf_run(f, n, X86_64, 41, 10) == deny, "seccomp x86_64: socket(AF_INET6) refused");
    ASSERT(bpf_run(f, n, X86_64, 41, 1) == allow, "seccomp x86_64: socket(AF_UNIX) allowed");
    ASSERT(bpf_run(f, n, X86_64, 0, 0) == allow, "seccomp x86_64: read allowed");
    ASSERT(bpf_run(f, n, X86_64, 425, 0) == deny, "seccomp x86_64: io_uring_setup refused");
    ASSERT(bpf_run(f, n, X86_64, 0x40000000u + 41, 2) == deny, "seccomp x86_64: an x32 call refused");
    ASSERT(bpf_run(f, n, I386, 102, 1) == deny, "seccomp i386 (int 0x80): socketcall refused");
    ASSERT(bpf_run(f, n, I386, 359, 2) == deny, "seccomp i386: socket(AF_INET) refused");
    ASSERT(bpf_run(f, n, I386, 359, 1) == allow, "seccomp i386: socket(AF_UNIX) allowed");
    ASSERT(bpf_run(f, n, I386, 425, 0) == deny, "seccomp i386: io_uring_setup refused");
    ASSERT(bpf_run(f, n, I386, 3, 0) == allow, "seccomp i386: read allowed");
    ASSERT(bpf_run(f, n, 0x12345678u, 41, 2) == kill, "seccomp: an unknown architecture is killed, not let through");
    n = tk_sc_no_net_filter(&tk_sc_aarch64, f);
    ASSERT(bpf_run(f, n, AARCH64, 198, 2) == deny, "seccomp aarch64: socket(AF_INET) refused");
    ASSERT(bpf_run(f, n, AARCH64, 198, 1) == allow, "seccomp aarch64: socket(AF_UNIX) allowed");
    ASSERT(bpf_run(f, n, AARCH64, 0x40000000u, 0) == allow, "seccomp aarch64: no x32 rule");
    ASSERT(bpf_run(f, n, ARM, 281, 10) == deny, "seccomp aarch32 compat: socket(AF_INET6) refused");
    ASSERT(bpf_run(f, n, ARM, 425, 0) == deny, "seccomp aarch32 compat: io_uring_setup refused");
    ASSERT(bpf_run(f, n, ARM, 102, 0) == allow, "seccomp aarch32 compat: 102 is not socketcall there");
    ASSERT(bpf_run(f, n, X86_64, 41, 2) == kill, "seccomp aarch64: an x86_64 call is killed");
}

/* ── The kernel half ──────────────────────────────────────────────── */

static int warned = 0;
static void warn_cb(const char *layer, const char *reason) {
    warned = 1;
    fprintf(stderr, "CAP004 (%s): %s\n", layer, reason);
}

/* Child: apply a read+write plan admitting `in` (a directory), then open
 * in/f, out.txt and in/new directly.  Exit code bits: 1 in/f opened,
 * 2 secret opened, 4 in/new created, 8 outside created, 16 the OS layer
 * reported itself partial. */
static int run_child(const char *in, const char *secret, const char *outside) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        TkOsPlan p;
        memset(&p, 0, sizeof p);
        char buf[PATH_MAX];
        p.read.on = p.write.on = 1;
        (void)tk_os_add_path(&p.read, in);
        (void)tk_os_add_path(&p.write, in);
        if (getcwd(buf, sizeof buf)) p.cwd = strdup(buf);
        tk_os_apply(&p, warn_cb);
        tk_os_plan_free(&p);
        int code = warned ? 16 : 0;
        snprintf(buf, sizeof buf, "%s/f", in);
        FILE *f = fopen(buf, "r");
        if (f) { code |= 1; fclose(f); }
        f = fopen(secret, "r");
        if (f) { code |= 2; fclose(f); }
        snprintf(buf, sizeof buf, "%s/new", in);
        f = fopen(buf, "w");
        if (f) { code |= 4; fclose(f); }
        f = fopen(outside, "w");
        if (f) { code |= 8; fclose(f); }
        _exit(code);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static void test_kernel(void) {
    char tmpl[PATH_MAX];
    const char *td = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/tk_cap_os_XXXXXX", td && *td ? td : "/tmp");
    char *d = mkdtemp(tmpl);
    if (!d) { printf("FAIL: mkdtemp\n"); failures++; return; }
    char root[PATH_MAX], in[PATH_MAX + 8], secret[PATH_MAX + 16], outside[PATH_MAX + 16], f[PATH_MAX + 16];
    if (!realpath(d, root)) { printf("FAIL: realpath\n"); failures++; return; }
    snprintf(in, sizeof in, "%s/in", root);
    snprintf(secret, sizeof secret, "%s/secret", root);
    snprintf(outside, sizeof outside, "%s/outside", root);
    snprintf(f, sizeof f, "%s/f", in);
    mkdir(in, 0700);
    FILE *w = fopen(f, "w");      if (w) { fputs("x", w); fclose(w); }
    w = fopen(secret, "w");       if (w) { fputs("s", w); fclose(w); }

    int c = run_child(in, secret, outside);
    if (c >= 0 && (c & 16) && !(c & ~(16 | 1 | 2 | 4 | 8)) && (c & 15) == 15) {
        printf("skip: kernel half -- the OS layer reported itself unavailable here (CAP004 above), "
               "and every open passed, as without it\n");
    } else {
        ASSERT(c >= 0 && !(c & 16), "kernel: the OS layer applied without a CAP004");
        ASSERT(c >= 0 && (c & 1), "kernel: a file inside the read scope opens");
        ASSERT(c >= 0 && !(c & 2), "kernel: a file outside the read scope does not open (no broker involved)");
        ASSERT(c >= 0 && (c & 4), "kernel: a file can be created inside the write scope");
        ASSERT(c >= 0 && !(c & 8), "kernel: a file outside the write scope cannot be created");
    }
    unlink(f);
    snprintf(f, sizeof f, "%s/new", in);
    unlink(f);
    unlink(secret);
    unlink(outside);
    rmdir(in);
    rmdir(root);
}

int main(void) {
    test_plan_text();
    test_seccomp_filter();
    test_kernel();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all cap_os tests passed\n");
    return 0;
}
