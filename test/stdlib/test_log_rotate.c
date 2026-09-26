/*
 * test_log_rotate.c — std.log access-log rotation loses no lines (127.191).
 *
 * Build and run: make test-stdlib-log
 *
 * The defect had three independent causes, and each test below isolates one:
 *
 *  A. same-second archives.  The rotated name had one-second resolution
 *     (stem.YYYYMMDD-HHMMSS.ext), so a second rotation inside the same second
 *     renamed onto — and gzipped over — the first archive.  One process,
 *     max_lines=5, 60 lines: every line must survive, in 12 archives.
 *
 *  B. pre-fork workers.  Each forked worker kept its own line count and its
 *     own FILE*.  When one rotated (rename + reopen) the others kept writing
 *     into the renamed-and-then-unlinked inode, and their counts were wrong
 *     for the shared file.  Four forked writers x 250 lines, max_lines=5: all
 *     1000 lines present exactly once, and every archive holds exactly 5
 *     lines (the count is of the SHARED file, not of one writer's share).
 *
 *  C. reopen.  The count restarted at 0 on every open, so a process restart
 *     let the live file grow to max_lines + whatever was already there.  3
 *     lines, close, reopen, 2 lines: that is 5 and must rotate.
 *
 *  D. tk_access_log_set_format rejects an unknown format (returns -1 and
 *     leaves the format unchanged) instead of ignoring it silently.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <zlib.h>
#include "../../src/stdlib/log.h"

static int failures = 0;

#define ASSERT(cond, ...) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); \
                        fprintf(stderr, "\n"); failures++; } \
         else { printf("pass: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXLINES 8192
static char *g_lines[MAXLINES];
static int   g_nlines;

static void collect_line(const char *s)
{
    if (g_nlines < MAXLINES) g_lines[g_nlines++] = strdup(s);
}

static void reset_lines(void)
{
    for (int i = 0; i < g_nlines; i++) free(g_lines[i]);
    g_nlines = 0;
}

/* Lines in a plain file; each is also collected. */
static int read_plain(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096]; int n = 0;
    while (fgets(buf, sizeof buf, f)) { collect_line(buf); n++; }
    fclose(f);
    return n;
}

/* Lines in a .gz archive; each is also collected. */
static int read_gz(const char *path)
{
    gzFile g = gzopen(path, "rb");
    if (!g) return -1;
    char buf[4096]; int n = 0;
    while (gzgets(g, buf, sizeof buf)) { collect_line(buf); n++; }
    gzclose(g);
    return n;
}

/* Walk dir: count the archives, check each holds exactly `per` lines (per<=0
 * skips that check), and collect every line of archives + live file. */
static void scan_dir(const char *dir, const char *live, int per,
                     int *archives, int *live_lines, int *bad_archives)
{
    *archives = 0; *bad_archives = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 3 || strcmp(e->d_name + L - 3, ".gz")) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int n = read_gz(p);
        (*archives)++;
        if (per > 0 && n != per) {
            (*bad_archives)++;
            fprintf(stderr, "  archive %s holds %d lines, expected %d\n", e->d_name, n, per);
        }
    }
    if (d) closedir(d);
    *live_lines = read_plain(live);
}

/* How many collected lines contain the marker exactly once in total. */
static int count_marker(const char *marker)
{
    int n = 0;
    for (int i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], marker)) n++;
    return n;
}

static void rm_tree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    if (d) closedir(d);
    rmdir(dir);
}

static void write_n(TkAccessLog *al, int worker, int n)
{
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "/w%d/i%d/", worker, i);
        tk_access_log_write(al, "127.0.0.1", "GET", path, 200, 10, NULL, "t");
    }
}

/* Every (worker, i) line present exactly once. */
static int all_present_once(int workers, int n)
{
    int missing = 0, dup = 0;
    for (int w = 0; w < workers; w++)
        for (int i = 0; i < n; i++) {
            char m[64]; snprintf(m, sizeof m, "/w%d/i%d/", w, i);
            int c = count_marker(m);
            if (c == 0) missing++;
            else if (c > 1) dup++;
        }
    if (missing || dup)
        fprintf(stderr, "  %d line(s) missing, %d duplicated\n", missing, dup);
    return !missing && !dup;
}

static void test_same_second(void)
{
    char dir[] = "/tmp/tk_logrot_A_XXXXXX";
    if (!mkdtemp(dir)) { ASSERT(0, "A: mkdtemp"); return; }
    char live[512]; snprintf(live, sizeof live, "%s/access.log", dir);

    TkAccessLog *al = tk_access_log_open(live, 5, 0, 0);
    ASSERT(al != NULL, "A: open");
    if (!al) return;
    write_n(al, 0, 60);
    tk_access_log_close(al);

    int arch, livel, bad;
    reset_lines();
    scan_dir(dir, live, 5, &arch, &livel, &bad);
    ASSERT(g_nlines == 60, "A: 60 lines written in one second, %d survive", g_nlines);
    ASSERT(all_present_once(1, 60), "A: every line present exactly once");
    ASSERT(arch == 12, "A: 12 archives, found %d", arch);
    ASSERT(bad == 0, "A: every archive holds exactly 5 lines");
    rm_tree(dir);
}

static void test_prefork(void)
{
    enum { WORKERS = 4, PER = 250 };
    char dir[] = "/tmp/tk_logrot_B_XXXXXX";
    if (!mkdtemp(dir)) { ASSERT(0, "B: mkdtemp"); return; }
    char live[512]; snprintf(live, sizeof live, "%s/access.log", dir);

    /* Opened BEFORE the fork, as a pre-fork server does. */
    TkAccessLog *al = tk_access_log_open(live, 5, 0, 0);
    ASSERT(al != NULL, "B: open before fork");
    if (!al) return;

    pid_t kids[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        kids[w] = fork();
        if (kids[w] == 0) { write_n(al, w, PER); _exit(0); }
    }
    int ok = 1;
    for (int w = 0; w < WORKERS; w++) {
        int st = 0;
        if (waitpid(kids[w], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) ok = 0;
    }
    ASSERT(ok, "B: %d workers exited cleanly", WORKERS);
    tk_access_log_close(al);

    int arch, livel, bad;
    reset_lines();
    scan_dir(dir, live, 5, &arch, &livel, &bad);
    ASSERT(g_nlines == WORKERS * PER, "B: %d lines written by %d workers, %d survive",
           WORKERS * PER, WORKERS, g_nlines);
    ASSERT(all_present_once(WORKERS, PER), "B: every line present exactly once");
    ASSERT(arch == WORKERS * PER / 5, "B: %d archives, found %d", WORKERS * PER / 5, arch);
    ASSERT(bad == 0, "B: every archive holds exactly 5 lines (count is of the shared file)");
    ASSERT(livel == 0, "B: live file empty after an exact multiple, holds %d", livel);
    rm_tree(dir);
}

static void test_reopen(void)
{
    char dir[] = "/tmp/tk_logrot_C_XXXXXX";
    if (!mkdtemp(dir)) { ASSERT(0, "C: mkdtemp"); return; }
    char live[512]; snprintf(live, sizeof live, "%s/access.log", dir);

    TkAccessLog *al = tk_access_log_open(live, 5, 0, 0);
    write_n(al, 0, 3);
    tk_access_log_close(al);
    al = tk_access_log_open(live, 5, 0, 0);
    ASSERT(al != NULL, "C: reopen");
    if (!al) return;
    write_n(al, 1, 2);
    tk_access_log_close(al);

    int arch, livel, bad;
    reset_lines();
    scan_dir(dir, live, 5, &arch, &livel, &bad);
    ASSERT(arch == 1, "C: 3 lines + reopen + 2 lines rotates once, found %d archive(s)", arch);
    ASSERT(livel == 0, "C: live file empty after the rotation, holds %d", livel);
    ASSERT(g_nlines == 5, "C: all 5 lines survive, %d do", g_nlines);
    rm_tree(dir);
}

static void test_set_format(void)
{
    ASSERT(tk_access_log_set_format("json") == 0, "D: json accepted");
    ASSERT(tk_access_log_set_format("combined") == 0, "D: combined accepted");
    ASSERT(tk_access_log_set_format("jsno") == -1, "D: unknown format rejected");
    ASSERT(tk_access_log_set_format(NULL) == -1, "D: null format rejected");
}

int main(void)
{
    test_same_second();
    test_prefork();
    test_reopen();
    test_set_format();
    reset_lines();
    if (failures == 0) { printf("All log rotation tests passed.\n"); return 0; }
    fprintf(stderr, "%d log rotation test(s) failed.\n", failures);
    return 1;
}
