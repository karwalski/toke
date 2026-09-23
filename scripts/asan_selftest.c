/* asan_selftest.c — the negative control for `make test-asan-*`.
 *
 * Story 123.9.
 *
 * A sanitizer job that reports "0 findings" is indistinguishable from a
 * sanitizer job whose instrumentation never got into the binary, and the
 * second is easy to arrive at by accident: a CFLAGS override that does not
 * reach the compile line, a link that picks up uninstrumented objects, a
 * -fsanitize= flag the driver accepted and dropped. `make test-asan-stdlib`
 * builds and runs THIS file first, and fails outright if it does not die.
 * So "0 findings" means the detector was watched to catch a planted defect
 * in this very build configuration, and then found nothing.
 *
 * Do not "fix" the defect below. It is the control.
 *
 * Build and run is done by the Makefile; the defect is a heap
 * use-after-free, which is the class the json.c reports in this epic fall
 * into and the one ASan is most reliable at.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* Opaque enough that -O1 cannot fold the whole thing away. */
    size_t n = strlen("0123456789ABCDEF");
    char *p = (char *)malloc(n + 1);
    if (!p) return 2;
    memcpy(p, "0123456789ABCDEF", n + 1);

    free(p);

    /* Deliberate heap-use-after-free. ASan must abort here. */
    volatile char c = p[3];

    printf("ASAN SELF-TEST FAILED: read '%c' from freed memory and lived. "
           "The sanitizer is NOT active in this build, so any 'clean' result "
           "from the targets that follow would be meaningless.\n", (char)c);
    /* Exit ZERO on the not-caught path, deliberately.
     *
     * The first version of this file returned 1 here, and the Makefile
     * treated "non-zero" as "the sanitizer caught it". That made the control
     * pass with the sanitizer switched off entirely -- the program simply
     * returned 1 by itself and the gate called it a catch. Caught 2026-09-24
     * by running the control against `SAN_FLAGS=-O1`.
     *
     * So: non-zero here means only "ASan aborted". And the Makefile does not
     * rely on that alone either -- it requires the AddressSanitizer banner in
     * the output. Two independent conditions, because exit status by itself
     * has now been wrong once. */
    return 0;
}
