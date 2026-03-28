/*
 * bench_stdlib.c — Performance benchmarks for all toke stdlib modules.
 *
 * Stories: 3.8.2
 * Branch:  feature/stdlib-3.8-bench
 *
 * Build: cc -O2 -Isrc/stdlib -o test/stdlib/bench_stdlib \
 *            test/stdlib/bench_stdlib.c \
 *            src/stdlib/str.c src/stdlib/json.c src/stdlib/file.c \
 *            src/stdlib/crypto.c src/stdlib/tk_time.c src/stdlib/process.c \
 *            src/stdlib/env.c src/stdlib/log.c src/stdlib/tk_test.c \
 *            src/stdlib/db.c src/stdlib/http.c -lsqlite3
 * Run:   ./test/stdlib/bench_stdlib
 *
 * Output format: BENCH <module.function> <N_iters> <total_ms> <avg_us>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "str.h"
#include "json.h"
#include "crypto.h"
#include "tk_time.h"
#include "process.h"
#include "env.h"
#include "log.h"

/* file.h redefines StrArray which str.h already defined — forward-declare
 * only what bench_file() needs to avoid the redefinition collision.
 * FileErrKind + FileErr must match file.h exactly for ABI compatibility. */
typedef enum { FILE_ERR_NOT_FOUND=1, FILE_ERR_PERMISSION, FILE_ERR_IO } BenchFileErrKind;
typedef struct { BenchFileErrKind kind; const char *msg; } BenchFileErr;
typedef struct { const char *ok; int is_err; BenchFileErr err; } BenchStrFileResult;
typedef struct { int ok;         int is_err; BenchFileErr err; } BenchBoolFileResult;
extern BenchStrFileResult  file_read(const char *path);
extern BenchBoolFileResult file_write(const char *path, const char *content);
extern BenchBoolFileResult file_delete(const char *path);
extern int                 file_exists(const char *path);

/* ── timing ─────────────────────────────────────────────────────────────── */

static uint64_t ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void report(const char *name, int n, uint64_t start_ns, uint64_t end_ns)
{
    double total_ms = (double)(end_ns - start_ns) / 1e6;
    double avg_us   = (double)(end_ns - start_ns) / 1e3 / (double)n;
    printf("BENCH %-40s %6d iters  %8.2f ms total  %8.3f us/iter\n",
           name, n, total_ms, avg_us);
}

/* ── C-native baselines ──────────────────────────────────────────────────── */

static void bench_c_strcpy(void)
{
    int n = 50000;
    char buf[64];
    uint64_t t0 = ns_now();
    for (int i = 0; i < n; i++) {
        strcpy(buf, "hello world");
        (void)buf;
    }
    report("c_native.strcpy[baseline]", n, t0, ns_now());
}

/* ── std.str ─────────────────────────────────────────────────────────────── */

static void bench_str(void)
{
    int n;
    uint64_t t0;

    n = 50000; t0 = ns_now();
    for (int i = 0; i < n; i++) str_concat("hello", " world");
    report("str.concat", n, t0, ns_now());

    n = 10000; t0 = ns_now();
    for (int i = 0; i < n; i++) str_split("a,b,c,d,e", ",");
    report("str.split", n, t0, ns_now());

    n = 50000; t0 = ns_now();
    for (int i = 0; i < n; i++) str_contains("the quick brown fox", "brown");
    report("str.contains", n, t0, ns_now());

    n = 50000; t0 = ns_now();
    for (int i = 0; i < n; i++) str_upper("hello world");
    report("str.upper", n, t0, ns_now());

    n = 50000; t0 = ns_now();
    for (int i = 0; i < n; i++) str_len("hello world");
    report("str.len", n, t0, ns_now());
}

/* ── std.json ────────────────────────────────────────────────────────────── */

static void bench_json(void)
{
    int n;
    uint64_t t0;
    const char *src = "{\"name\":\"alice\",\"age\":30,\"score\":9.5}";
    JsonResult jr = json_dec(src);
    if (jr.is_err) { printf("SKIP  json (parse failed)\n"); return; }
    Json j = jr.ok;

    n = 5000; t0 = ns_now();
    for (int i = 0; i < n; i++) json_dec(src);
    report("json.dec", n, t0, ns_now());

    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) json_str(j, "name");
    report("json.str", n, t0, ns_now());

    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) json_u64(j, "age");
    report("json.u64", n, t0, ns_now());

    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) json_enc("hello world");
    report("json.enc", n, t0, ns_now());
}

/* ── std.file ────────────────────────────────────────────────────────────── */

static void bench_file(void)
{
    int n;
    uint64_t t0;
    const char *path = "/tmp/tk_bench_file.txt";

    n = 500; t0 = ns_now();
    for (int i = 0; i < n; i++) {
        file_write(path, "bench data");
        (void)file_read(path);
        file_delete(path);
    }
    report("file.write+read+delete", n, t0, ns_now());

    n = 2000; t0 = ns_now();
    for (int i = 0; i < n; i++) file_exists("/tmp");
    report("file.exists", n, t0, ns_now());
}

/* ── std.crypto ──────────────────────────────────────────────────────────── */

static void bench_crypto_sha256_c_native(void)
{
    /* C-native baseline: raw memcpy as proxy for equivalent work */
    int n = 5000;
    const char *msg = "the quick brown fox jumps over the lazy dog";
    char buf[64];
    uint64_t t0 = ns_now();
    for (int i = 0; i < n; i++) {
        memcpy(buf, msg, strlen(msg));
        (void)buf;
    }
    report("c_native.sha256_memcpy[baseline]", n, t0, ns_now());
}

static void bench_crypto(void)
{
    int n;
    uint64_t t0;
    const uint8_t msg[] = "the quick brown fox jumps over the lazy dog";
    ByteArray ba = { msg, sizeof(msg) - 1 };
    ByteArray key = { (const uint8_t *)"secret", 6 };

    n = 5000; t0 = ns_now();
    for (int i = 0; i < n; i++) crypto_sha256(ba);
    report("crypto.sha256", n, t0, ns_now());

    n = 5000; t0 = ns_now();
    for (int i = 0; i < n; i++) crypto_hmac_sha256(key, ba);
    report("crypto.hmac_sha256", n, t0, ns_now());

    ByteArray digest = crypto_sha256(ba);
    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) crypto_to_hex(digest);
    report("crypto.to_hex", n, t0, ns_now());

    bench_crypto_sha256_c_native();
}

/* ── std.time ────────────────────────────────────────────────────────────── */

static void bench_time(void)
{
    int n;
    uint64_t t0;

    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) tk_time_now();
    report("time.now", n, t0, ns_now());

    uint64_t base = tk_time_now();
    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) tk_time_since(base);
    report("time.since", n, t0, ns_now());

    n = 5000; t0 = ns_now();
    for (int i = 0; i < n; i++) tk_time_format(base, "%Y-%m-%d %H:%M:%S");
    report("time.format", n, t0, ns_now());
}

/* ── std.env ─────────────────────────────────────────────────────────────── */

static void bench_env(void)
{
    int n;
    uint64_t t0;

    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) env_get("PATH");
    report("env.get (present)", n, t0, ns_now());

    n = 20000; t0 = ns_now();
    for (int i = 0; i < n; i++) env_get_or("TOKE_BENCH_ABSENT", "default");
    report("env.get_or (absent)", n, t0, ns_now());
}

/* ── std.process ─────────────────────────────────────────────────────────── */

static void bench_process(void)
{
    int n;
    uint64_t t0;

    n = 50; t0 = ns_now();
    for (int i = 0; i < n; i++) {
        const char *args[] = { "true", NULL };
        SpawnResult sr = process_spawn(args);
        if (!sr.is_err) process_wait(sr.ok);
    }
    report("process.spawn+wait(true)", n, t0, ns_now());
}

/* ── std.log ─────────────────────────────────────────────────────────────── */

static void bench_log(void)
{
    /* Redirect stderr to /dev/null for benchmark so output doesn't dominate */
    FILE *old_stderr = stderr;
    (void)old_stderr;
    freopen("/dev/null", "w", stderr);

    int n;
    uint64_t t0;

    n = 10000; t0 = ns_now();
    const char *fields[] = { "key", "val" };
    for (int i = 0; i < n; i++) tk_log_info("bench message", fields, 2);
    report("log.info (2 fields, to /dev/null)", n, t0, ns_now());

    freopen("/dev/tty", "w", stderr);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== toke stdlib performance benchmarks ===\n");
    printf("Format: BENCH <function> <N> <total_ms> <avg_us/iter>\n");
    printf("Regression threshold: >20%% vs baseline commits to benchmark/results/\n\n");

    bench_c_strcpy();
    printf("\n");

    bench_str();
    printf("\n");

    bench_json();
    printf("\n");

    bench_file();
    printf("\n");

    bench_crypto();
    printf("\n");

    bench_time();
    printf("\n");

    bench_env();
    printf("\n");

    bench_process();
    printf("\n");

    bench_log();
    printf("\n");

    printf("=== benchmarks complete ===\n");
    return 0;
}
