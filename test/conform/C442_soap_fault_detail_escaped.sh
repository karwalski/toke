#!/usr/bin/env bash
# C442_soap_fault_detail_escaped.sh — soap_fault XML-escapes its detail, as
# it already did faultcode and faultstring (story 121.16: PAR-11).
#
# THE DEFECT (measured on main before 121.16).
#   soap_fault escaped faultcode and faultstring but wrote detail raw, so a
#   detail carrying request data -- `</detail><injected/><detail>` -- closed
#   the element and added its own elements to the fault envelope.
#
# WHAT THIS DRIVES (ASan+UBSan build of soap.c):
#   an injecting detail -> exactly one <detail> open and one </detail>
#   close, no <injected/> element, the payload present escaped; each of
#   & < > " ' escaped; an empty or NULL detail omits <detail>; faultcode and
#   faultstring unchanged (control); the fault still parses as one element
#   (python3's xml.etree, whose only children are faultcode, faultstring,
#   detail).
#
# Negative control: C442_SOAP_C=<file> builds against another soap.c;
# main's fails the injection, escaping and parse checks.
#
# Story: 121.16

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
SOAP_C="${C442_SOAP_C:-${S}/soap.c}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c442_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C442: soap_fault escapes its detail"
echo "-----------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soap.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static int count(const char *h, const char *n) {
    int c = 0; size_t L = strlen(n);
    for (const char *p = strstr(h, n); p; p = strstr(p + L, n)) c++;
    return c;
}

int main(int argc, char **argv) {
    const char *f = soap_fault("Client", "Bad <input>", "</detail><injected/><detail>");
    CHECK(count(f, "<detail>") == 1 && count(f, "</detail>") == 1 && !strstr(f, "<injected/>"),
          "a detail with </detail><injected/> cannot close the element or add one");
    CHECK(strstr(f, "&lt;/detail&gt;&lt;injected/&gt;&lt;detail&gt;") != NULL,
          "the payload is present, escaped");
    if (argc > 1) { FILE *o = fopen(argv[1], "w"); fputs(f, o); fclose(o); }
    free((char *)f);

    f = soap_fault("c", "s", "a&b<c>d\"e'f");
    CHECK(strstr(f, "<detail>a&amp;b&lt;c&gt;d&quot;e&apos;f</detail>") != NULL,
          "each of & < > \" ' is escaped in detail");
    free((char *)f);

    f = soap_fault("c", "s", "");
    CHECK(strstr(f, "<detail>") == NULL, "an empty detail omits <detail>");
    free((char *)f);
    f = soap_fault("c", "s", NULL);
    CHECK(strstr(f, "<detail>") == NULL, "a NULL detail omits <detail>");
    free((char *)f);

    f = soap_fault("Se<rver", "Internal & Error", "plain");
    CHECK(strstr(f, "<faultcode>Se&lt;rver</faultcode>") &&
          strstr(f, "<faultstring>Internal &amp; Error</faultstring>") &&
          strstr(f, "<detail>plain</detail>"),
          "control: faultcode/faultstring escaping and a plain detail unchanged");
    free((char *)f);

    printf("\n");
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" \
        "${WORK}/t.c" "${SOAP_C}" >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" "${WORK}/fault.xml"
rc=$?

if command -v python3 >/dev/null 2>&1; then
    if python3 - "${WORK}/fault.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
src = open(sys.argv[1]).read().replace("soap:Fault", "Fault")
root = ET.fromstring(src)
tags = [c.tag for c in root]
sys.exit(0 if tags == ["faultcode", "faultstring", "detail"] else 1)
PY
    then echo "  PASS: the injected fault parses as one Fault with children faultcode, faultstring, detail"
    else echo "  FAIL: the injected fault parses as one Fault with children faultcode, faultstring, detail"; rc=1
    fi
else
    echo "  SKIP: python3 not found (XML parse check)"
fi
echo ""
[ "${rc}" -eq 0 ] && echo "C442: PASS" || echo "C442: FAIL (exit ${rc})"
exit "${rc}"
