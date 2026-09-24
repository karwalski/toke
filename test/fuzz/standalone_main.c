/*
 * standalone_main.c — replay driver for the libFuzzer harnesses (123.10).
 *
 * Every harness in test/fuzz/ exports LLVMFuzzerTestOneInput().  With
 * -fsanitize=fuzzer, libFuzzer supplies main() and mutates inputs.  A
 * toolchain without the libFuzzer runtime (Apple clang on this project's
 * development Macs ships no libclang_rt.fuzzer_osx.a) cannot link that, so
 * these harnesses were built by no target at all.  Linked against THIS file
 * instead, each harness becomes a plain program that replays every file it
 * is given — a regression run over the seed and adversarial corpora, under
 * ASAN+UBSAN, with no mutation.  It proves the harness builds, links and runs
 * clean on known inputs; it is not fuzzing.  CI on Linux builds the same
 * harnesses in libFuzzer mode (FUZZ_ENGINE=libfuzzer) and fuzzes for real.
 *
 * Usage:  fuzz-xxx <file-or-directory>...
 *   Directories are read one level deep (no recursion), in sorted order.
 *   Exit 0 when every input was replayed; 1 on an unreadable argument or when
 *   no input at all was replayed (a run over nothing proves nothing).  A
 *   sanitizer finding aborts the process with the sanitizer's own report.
 */

#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static long replayed;

static int replay_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fuzz-replay: cannot open %s\n", path); return 1; }
    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(f); return 1; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return 1; }
            buf = nb; cap *= 2;
        }
    }
    fclose(f);
    /* Hand libFuzzer's contract to the harness: an exactly-sized heap copy,
     * so ASAN catches a read one byte past the end. */
    uint8_t *exact = malloc(len ? len : 1);
    if (!exact) { free(buf); return 1; }
    memcpy(exact, buf, len);
    free(buf);
    LLVMFuzzerTestOneInput(exact, len);
    free(exact);
    replayed++;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int replay_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "fuzz-replay: cannot open %s\n", dir); return 1; }
    char **names = NULL;
    size_t count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            char **nn = realloc(names, cap * sizeof *names);
            if (!nn) break;
            names = nn;
        }
        names[count++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, count, sizeof *names, cmp_str);
    int rc = 0;
    for (size_t i = 0; i < count; i++) {
        size_t plen = strlen(dir) + strlen(names[i]) + 2;
        char *p = malloc(plen);
        if (p) {
            struct stat st;
            snprintf(p, plen, "%s/%s", dir, names[i]);
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) rc |= replay_file(p);
            free(p);
        }
        free(names[i]);
    }
    free(names);
    return rc;
}

int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "fuzz-replay: no such input: %s\n", argv[i]);
            rc = 1;
        } else if (S_ISDIR(st.st_mode)) {
            rc |= replay_dir(argv[i]);
        } else {
            rc |= replay_file(argv[i]);
        }
    }
    printf("fuzz-replay: %s: %ld input(s) replayed\n", argv[0], replayed);
    if (replayed == 0) {
        fprintf(stderr, "fuzz-replay: nothing was replayed -- not a pass\n");
        return 1;
    }
    return rc;
}
