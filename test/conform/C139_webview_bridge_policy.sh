#!/usr/bin/env bash
# C139_webview_bridge_policy.sh — the webview bridge trusts only named origins
# (story 122.11).
#
# THE DEFECT.  webview.c exposed its native bridge to any page the view
# reached.  The window.toke bootstrap was injected with forMainFrameOnly:NO
# and the WKScriptMessageHandler never looked at who sent a message, so a
# redirect target, a clicked link or a third-party iframe could call every
# registered handler.  tk_webview_eval_js evaluated into whatever document was
# loaded, and the loke:// scheme handler answered requests initiated by any
# page.  Handler names were spliced into JavaScript unchecked, the reply was
# escaped for `"` and `\` only (a newline in a reply was a JS syntax error)
# and cut at 8 KiB, and the bootstrap was built with the
# `pos += snprintf(buf+pos, sizeof buf - pos, ...)` pattern into a 16 KiB
# stack buffer, which runs off the stack once enough handlers exist.
#
# WHAT THIS DRIVES (headless; built with -fsanitize=address,undefined):
#   the platform-independent policy in webview.c --
#     tk_webview_origin_parse    scheme/host/port, userinfo stripped at the
#                                LAST '@', default ports normalised, IPv6,
#                                malformed input refused;
#     tk_webview_origin_trusted  loke:/file: trusted; about:/data:/remote
#                                denied unless allow-listed; exact
#                                scheme+host+port match (no suffix match);
#     tk_webview_handler_name_ok identifiers only (plus "loke_scheme");
#     tk_webview_js_escape       quote, backslash, controls, <, >, U+2028/9
#                                escaped; a 100 KiB reply is not truncated;
#   and tk_webview_allow_origin on an invalid handle.
#
# WHAT THIS CANNOT DRIVE: the WebKit side -- that a message from a sub-frame
# or an untrusted origin is dropped, that eval_js returns 0 on a remote page,
# that a loke:// load from a remote page fails -- needs a window, a display
# and an NSApplication run loop.  Those paths call exactly the functions
# tested here (tk_message_trusted / tk_page_trusted / tk_nsurl_trusted);
# they are compiled on macOS by this script and exercised by no test.
#
# Negative control: drop the userinfo strip in tk_webview_origin_parse ->
# "userinfo does not smuggle a trusted host"; make tk_webview_origin_trusted
# return 1 for any non-empty host -> "https://evil.example denied";
# drop the `c < 0x20` escape -> "newline escaped".
#
# Story: 122.11

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c139_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C139: webview bridge trust policy"
echo "---------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webview.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static int parse_is(const char *url, const char *sch, const char *host, int port) {
    TkWebviewOrigin o;
    return tk_webview_origin_parse(url, &o) && !strcmp(o.scheme, sch) &&
           !strcmp(o.host, host) && o.port == port;
}

static int trusted(const char *url, const TkWebviewOrigin *allow, int n) {
    TkWebviewOrigin o;
    if (!tk_webview_origin_parse(url, &o)) return 0;
    return tk_webview_origin_trusted(&o, allow, n);
}

static int esc_is(const char *in, const char *want) {
    char *e = tk_webview_js_escape(in);
    int ok = e && !strcmp(e, want);
    if (!ok) printf("    got [%s] want [%s]\n", e ? e : "(null)", want);
    free(e);
    return ok;
}

int main(void) {
    TkWebviewOrigin o;

    /* ── parse ── */
    CHECK(parse_is("https://App.Example.COM/x?y#z", "https", "app.example.com", 0),
          "parse lower-cases scheme and host, drops path");
    CHECK(parse_is("https://a.example:443/", "https", "a.example", 0), "https :443 is the default port");
    CHECK(parse_is("http://a.example:80", "http", "a.example", 0), "http :80 is the default port");
    CHECK(parse_is("https://a.example:8443/", "https", "a.example", 8443), "explicit port kept");
    CHECK(parse_is("https://trusted.example@evil.example/", "https", "evil.example", 0),
          "userinfo does not smuggle a trusted host");
    CHECK(parse_is("https://u:p@x@evil.example:1/", "https", "evil.example", 1),
          "userinfo ends at the LAST @");
    CHECK(parse_is("http://[::1]:8080/", "http", "[::1]", 8080), "IPv6 literal with port");
    CHECK(parse_is("loke://app/index.html", "loke", "app", 0), "loke origin");
    CHECK(parse_is("file:///Users/x/a.html", "file", "", 0), "file origin, empty host");
    CHECK(parse_is("about:blank", "about", "", 0), "about:blank has no host");
    CHECK(!tk_webview_origin_parse("https://a.example:99999/", &o), "port > 65535 refused");
    CHECK(!tk_webview_origin_parse("https://a.example:8x/", &o), "non-digit port refused");
    CHECK(!tk_webview_origin_parse("", &o), "empty string refused");
    CHECK(!tk_webview_origin_parse("1http://a/", &o), "scheme must start with a letter");
    CHECK(!tk_webview_origin_parse("noscheme", &o), "no ':' refused");
    CHECK(!tk_webview_origin_parse(NULL, &o), "NULL refused");

    /* ── trust ── */
    TkWebviewOrigin allow[2];
    CHECK(tk_webview_origin_parse("https://app.example.com", &allow[0]), "allow entry parses");
    CHECK(tk_webview_origin_parse("http://localhost:5173/", &allow[1]), "allow entry 2 parses");
    CHECK(trusted("loke://app/", NULL, 0), "loke:// trusted by default");
    CHECK(trusted("file:///tmp/x.html", NULL, 0), "file:// trusted by default");
    CHECK(!trusted("https://evil.example/", NULL, 0), "https://evil.example denied with no allowlist");
    CHECK(!trusted("https://app.example.com/", NULL, 0), "an unlisted https origin is denied");
    CHECK(!trusted("about:blank", allow, 2), "about:blank denied");
    CHECK(!trusted("data:text/html,<b>x</b>", allow, 2), "data: denied");
    CHECK(!trusted("javascript:alert(1)", allow, 2), "javascript: denied");
    CHECK(trusted("https://app.example.com:443/p", allow, 2), "allow-listed origin trusted");
    CHECK(trusted("https://APP.example.com/", allow, 2), "host match is case-insensitive");
    CHECK(!trusted("http://app.example.com/", allow, 2), "scheme must match (http != https)");
    CHECK(!trusted("https://app.example.com:8443/", allow, 2), "port must match");
    CHECK(!trusted("https://app.example.com.evil.example/", allow, 2), "no suffix match");
    CHECK(!trusted("https://evil.example/app.example.com", allow, 2), "path is not the origin");
    CHECK(!trusted("https://app.example.com@evil.example/", allow, 2), "userinfo trick denied");
    CHECK(trusted("http://localhost:5173/x", allow, 2), "dev origin with port trusted when listed");
    CHECK(!trusted("http://localhost/", allow, 2), "same host, other port denied");
    CHECK(!tk_webview_origin_trusted(NULL, allow, 2), "NULL origin denied");

    /* ── handler names ── */
    CHECK(tk_webview_handler_name_ok("save"), "name 'save' ok");
    CHECK(tk_webview_handler_name_ok("readFile2"), "name 'readFile2' ok");
    CHECK(tk_webview_handler_name_ok("loke_scheme"), "reserved 'loke_scheme' ok");
    CHECK(!tk_webview_handler_name_ok("a'];alert(1);//"), "JS-breaking name refused");
    CHECK(!tk_webview_handler_name_ok("a_b"), "underscore refused");
    CHECK(!tk_webview_handler_name_ok(""), "empty name refused");
    CHECK(!tk_webview_handler_name_ok("1x"), "leading digit refused");
    CHECK(!tk_webview_handler_name_ok(NULL), "NULL name refused");
    char n64[66], n65[67];
    memset(n64, 'a', 64); n64[64] = '\0';
    memset(n65, 'a', 65); n65[65] = '\0';
    CHECK(tk_webview_handler_name_ok(n64), "64-char name ok");
    CHECK(!tk_webview_handler_name_ok(n65), "65-char name refused");

    /* ── reply escaping ── */
    CHECK(esc_is("plain", "plain"), "plain text unchanged");
    CHECK(esc_is("a\"b\\c", "a\\\"b\\\\c"), "quote and backslash escaped");
    CHECK(esc_is("x\ny", "x\\u000ay"), "newline escaped");
    CHECK(esc_is("\r\t", "\\u000d\\u0009"), "CR and TAB escaped");
    CHECK(esc_is("</script>", "\\u003c/script\\u003e"), "< and > escaped");
    CHECK(esc_is("a\xE2\x80\xA8" "b\xE2\x80\xA9", "a\\u2028b\\u2029"), "U+2028/U+2029 escaped");
    CHECK(esc_is("caf\xC3\xA9", "caf\xC3\xA9"), "other UTF-8 passes through");
    CHECK(esc_is(NULL, ""), "NULL escapes to empty");
    size_t big = 100 * 1024;
    char *s = malloc(big + 1);
    memset(s, '"', big); s[big] = '\0';
    char *e = tk_webview_js_escape(s);
    CHECK(e && strlen(e) == 2 * big, "100 KiB reply escaped in full (no 8 KiB cut)");
    free(e); free(s);

    /* ── API on an invalid handle ── */
    TkWebviewHandle bad = { NULL };
    CHECK(tk_webview_allow_origin(&bad, "https://a.example") == 0, "allow_origin(invalid handle) = 0");
    CHECK(tk_webview_allow_origin(NULL, "https://a.example") == 0, "allow_origin(NULL) = 0");

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

LINK=""
if [ "$(uname -s)" = Darwin ]; then
    LINK="-framework WebKit -framework Cocoa -lobjc"
fi
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -g -O1 ${SAN} \
        -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" "${S}/webview.c" ${LINK} \
        >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
echo "  PASS: webview.c built as plain C99 with ${SAN}"
ASAN_OPTIONS="detect_leaks=${C139_LEAKS:-0}:abort_on_error=0" "${WORK}/t"
