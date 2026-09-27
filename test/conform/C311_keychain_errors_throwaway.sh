#!/usr/bin/env bash
# C311_keychain_errors_throwaway.sh — std.keychain tells "no item" from "the
# query failed", and a program can work in a throwaway keychain (story 138.4;
# loke handoff B4: R4, R5).
#
# THE DEFECTS.
#   R4. keychain.c mapped every non-success OSStatus to NULL / 0: a locked or
#       denied keychain read exactly like an absent item, so loke's first-use
#       branch ran against a store it could not read.
#   R5. No call named a keychain, so every test wrote to the user's LOGIN
#       keychain.
#
# WHAT IS ASSERTED.  This suite never touches the login keychain: every item
# operation runs after keychain_use(<a keychain this suite created in its own
# temp dir>), and the only other keychain it names is the System keychain, and
# only to prove keychain_destroy refuses it.
#   Part 1 (C driver, keychain.c under ASan+UBSan):
#     - create two keychains; use A; set / exists / get round-trip
#     - use B: exists / get / delete are false AND lasterrkind is "notfound",
#       laststatus -25300 (errSecItemNotFound)
#     - lock A (SecKeychainLock) and use it: get and set fail with lasterrkind
#       "locked" -- NOT "notfound" -- and no dialog is shown (interaction is
#       disallowed while a named keychain is selected); exists still answers
#       true, because a locked file keychain's item attributes stay readable
#     - unlock A: get returns the secret again
#     - keychain_destroy on the System keychain is refused ("denied")
#     - bad arguments are "badarg"; a missing keychain is "nokeychain"
#     - the user's keychain search list is the same before and after
#   Part 2 (a toke program, end to end): the same through keychain.create /
#     use / set / get / exists / delete / lasterrkind / laststatus / destroy.
#
# Negative control: C311_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-09-27 with a copy of this tree whose kc_status() maps every
# failure to "notfound" (the pre-138.4 conflation): 4 of 30 fail -- both
# "locked" cases, "nokeychain", and part 2.  Main's keychain.c (c15dd11)
# cannot link part 1: it has no keychain_use / keychain_last_kind.
#
# Skips (named) on a host without Security.framework.
#
# Story: 138.4
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C311_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
echo "C311: std.keychain error channel + throwaway keychains (138.4)"
echo "---------------------------------------------------------------"
if [ "$(uname -s)" != "Darwin" ]; then
    echo "SKIP: C311 needs macOS Security.framework (host is $(uname -s))"
    exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c311_XXXXXX")"; WORK="$(cd "${WORK}" && pwd -P)"
trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
security list-keychains > searchlist.before 2>&1

cat > t.c <<'C'
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/Security.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "keychain.h"
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s  [kind=%s status=%ld]\n", m, \
                                keychain_last_kind(), keychain_last_status()); fails++; } } while (0)
#define KIND(k) (strcmp(keychain_last_kind(), (k)) == 0)
int main(int argc, char **argv) {
    (void)argc;
    char a[1024], b[1024];
    snprintf(a, sizeof a, "%s/a.keychain", argv[1]);
    snprintf(b, sizeof b, "%s/b.keychain", argv[1]);
    const char *S = "tk.c311.svc", *A = "acct";
    CHECK(keychain_create(a, "pw-a") && keychain_create(b, "pw-b"), "create two throwaway keychains");
    CHECK(!keychain_create(a, "pw-a") && keychain_last_status() != 0, "creating one that exists fails");
    CHECK(keychain_use(a) && KIND(""), "use(a)");
    CHECK(keychain_set(S, A, "s3cret") && KIND(""), "set in a");
    CHECK(keychain_exists(S, A) == 1 && KIND(""), "exists in a");
    char *g = keychain_get(S, A);
    CHECK(g && !strcmp(g, "s3cret") && KIND(""), "get in a returns the secret");
    free(g);

    CHECK(keychain_use(b), "use(b)");
    CHECK(keychain_exists(S, A) == 0 && KIND("notfound") && keychain_last_status() == -25300,
          "b: exists is false, kind notfound, status -25300");
    CHECK(keychain_get(S, A) == NULL && KIND("notfound"), "b: get is empty, kind notfound");
    CHECK(keychain_delete(S, A) == 0 && KIND("notfound"), "b: delete is false, kind notfound");

    /* R4: a locked keychain is a failed query, not an absent item. */
    SecKeychainRef ka = NULL;
    CHECK(SecKeychainOpen(a, &ka) == errSecSuccess && SecKeychainLock(ka) == errSecSuccess, "lock a");
    CHECK(keychain_use(a), "use(a) while locked");
    CHECK(keychain_get(S, A) == NULL && KIND("locked") && keychain_last_status() != 0,
          "locked: get is empty and kind is locked, not notfound");
    /* A file-based keychain's item ATTRIBUTES are readable while it is
     * locked (measured: SecItemCopyMatching without kSecReturnData succeeds),
     * so exists answers truthfully rather than failing. */
    CHECK(keychain_exists(S, A) == 1 && KIND(""), "locked: exists still answers true (attributes are not locked)");
    CHECK(keychain_set(S, A, "x") == 0 && KIND("locked"), "locked: set is false and kind is locked");
    CHECK(strlen(keychain_last_message()) > 0 && !strstr(keychain_last_message(), "s3cret"),
          "the message is non-empty and carries no secret");
    CHECK(SecKeychainUnlock(ka, 4, "pw-a", true) == errSecSuccess, "unlock a");
    g = keychain_get(S, A);
    CHECK(g && !strcmp(g, "s3cret") && KIND(""), "unlocked: get returns the secret again");
    free(g);
    CHECK(keychain_delete(S, A) == 1 && KIND(""), "delete in a");
    CFRelease(ka);

    CHECK(!keychain_destroy("/Library/Keychains/System.keychain") && KIND("denied"),
          "destroy refuses a keychain on the user's search list (System)");
    struct stat st;
    CHECK(stat("/Library/Keychains/System.keychain", &st) == 0, "... and it is still there");
    CHECK(keychain_get(NULL, A) == NULL && KIND("badarg"), "null service is badarg");
    CHECK(!keychain_set(S, A, "") && KIND("badarg"), "empty secret is badarg");
    CHECK(!keychain_create("relative.keychain", "pw") && KIND("badarg"), "a relative path is badarg");
    CHECK(keychain_destroy(a) && keychain_destroy(b) && stat(a, &st) != 0, "destroy both throwaway keychains");
    CHECK(!keychain_use(a) && KIND("nokeychain"), "use of a destroyed keychain is nokeychain");
    CHECK(keychain_use("") && KIND(""), "use(\"\") returns to the default (nothing is read from it here)");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o t t.c "${S}/keychain.c" \
        -framework Security -framework CoreFoundation > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    mkdir kc1
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t "${WORK}/kc1" > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"
        grep -a 'ERROR: AddressSanitizer\|runtime error' t.out | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── Part 2: end to end ───────────────────────────────────────────────────
mkdir kc2
cat > k.tk <<'TOKE'
m=main;i=io:std.io;i=kc:std.keychain;i=str:std.str;i=env:std.env;
f=main():i64{
  let d=env.get("C311DIR");
  let pa=str.concat(d;"/a.keychain");
  let pb=str.concat(d;"/b.keychain");
  let c1=kc.create(pa;"pw");
  let c2=kc.create(pb;"pw");
  let u1=kc.use(pa);
  let s1=kc.set("tk.c311";"acct";"s3cret");
  let e1=kc.exists("tk.c311";"acct");
  let g1=kc.get("tk.c311";"acct");
  let u2=kc.use(pb);
  let e2=kc.exists("tk.c311";"acct");
  let k2=kc.lasterrkind();
  let st=kc.laststatus();
  let u3=kc.use(pa);
  let del=kc.delete("tk.c311";"acct");
  let del2=kc.delete("tk.c311";"acct");
  let k4=kc.lasterrkind();
  let x=kc.destroy(pa);
  let y=kc.destroy(pb);
  let u4=kc.use(pa);
  let k5=kc.lasterrkind();
  io.println("create=\(c1)\(c2) set=\(s1) exists-a=\(e1) len-a=\(str.len(g1)) exists-b=\(e2) kind=\(k2) status=\(st) del=\(del) del2=\(del2) kind-del2=\(k4) destroy=\(x)\(y) use-gone=\(u4) kind=\(k5)");
  <0
};
TOKE
want="create=11 set=1 exists-a=1 len-a=6 exists-b=0 kind=notfound status=-25300 del=1 del2=0 kind-del2=notfound destroy=11 use-gone=0 kind=nokeychain"
if TKC_STDLIB_DIR="${S}" "${TKC}" --allow-all --out k k.tk > k.log 2>&1; then
    got="$(C311DIR="${WORK}/kc2" ./k --allow-all 2>&1)"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: toke program: ${want}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: toke program printed '${got}'"; echo "        expected '${want}'"; FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: k.tk did not compile"; sed 's/^/        /' k.log | head -4; FAIL=$((FAIL + 1))
fi

security list-keychains > searchlist.after 2>&1
if cmp -s searchlist.before searchlist.after; then
    echo "  PASS: the user's keychain search list is unchanged"; PASS=$((PASS + 1))
else
    echo "  FAIL: the keychain search list changed:"; diff searchlist.before searchlist.after | sed 's/^/        /'
    FAIL=$((FAIL + 1))
fi

echo "---------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
