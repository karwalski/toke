/* SPDX-License-Identifier: MIT */
/*
 * soap.c — SOAP 1.1/1.2 envelope builder, fault generator, extractor,
 *           and WS-Security header stubs for the std.soap module.
 *
 * Pure C implementation. Uses snprintf + malloc for string building.
 * No external dependencies beyond libc.
 *
 * 127.295 (S1 step 5, runtime-abi §3.1): every string handed out -- the
 * envelopes, faults, extracted body/fault and WS-Security blocks -- is a
 * sealed string from the string heap (tk_strhdr.h), and inputs are measured
 * with tk_strlen.  A result is never freed, written or reallocated, C
 * callers included.  The escaped copies xml_escape makes are this file's
 * own scratch and stay malloc'd.
 *
 * Stories: 88.6.3, 88.6.5
 */

#include "soap.h"
#include "tk_strhdr.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdio.h>

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Safe pointer that treats NULL as "" */
static const char *safe_str(const char *s) {
    return s ? s : "";
}

/* Return 1 if s is NULL or empty */
static int is_empty(const char *s) {
    return !s || s[0] == '\0';
}

/* A sealed "" -- what every producer returns when it cannot build its
 * result.  The literal is the last resort (a correct, unsealed C string). */
static const char *sealed_empty(void) {
    char *e = tks_dup("", 0);
    return e ? e : "";
}

/* Hand the n-byte malloc'd string buf to the caller as one sealed copy and
 * free buf.  buf holds no 0 byte before n: it was built by snprintf from C
 * strings.  If the copy cannot be made, buf itself is returned (correct,
 * unsealed, never freed). */
static const char *seal_take(char *buf, size_t n) {
    char *s = tks_dup(buf, n);
    if (!s) return buf;
    free(buf);
    return s;
}

/*
 * xml_escape — escape &, <, >, ", ' for safe XML embedding.
 * Returns a malloc'd string the caller frees (this file's scratch), its
 * length in *out_len; NULL when it cannot be allocated.
 */
static char *xml_escape(const char *s, size_t *out_len) {
    *out_len = 0;
    if (!s) s = "";
    size_t len = tk_strlen(s);
    /* Worst case: every char becomes &quot; (6 chars) */
    size_t cap = len * 6 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
            case '&':  memcpy(out + pos, "&amp;", 5);  pos += 5; break;
            case '<':  memcpy(out + pos, "&lt;", 4);   pos += 4; break;
            case '>':  memcpy(out + pos, "&gt;", 4);   pos += 4; break;
            case '"':  memcpy(out + pos, "&quot;", 6); pos += 6; break;
            case '\'': memcpy(out + pos, "&apos;", 6); pos += 6; break;
            default:   out[pos++] = s[i]; break;
        }
    }
    out[pos] = '\0';
    *out_len = pos;
    return out;
}

/*
 * find_tag_content — find content between <prefix...> and </tag>.
 * Searches for the opening tag prefix and closing tag, copies the
 * content between them into a sealed string.
 * Returns NULL if not found.
 */
static char *find_tag_content(const char *xml, const char *open_tag,
                              const char *close_tag) {
    if (!xml || !open_tag || !close_tag) return NULL;

    const char *start = strstr(xml, open_tag);
    if (!start) return NULL;

    /* Move past the opening tag (find the closing '>') */
    const char *content_start = strchr(start, '>');
    if (!content_start) return NULL;
    content_start++; /* skip '>' */

    const char *end = strstr(content_start, close_tag);
    if (!end) return NULL;

    return tks_dup(content_start, (size_t)(end - content_start));
}

/* ── SOAP 1.1 Namespace ───────────────────────────────────────────────── */

#define SOAP11_NS "http://schemas.xmlsoap.org/soap/envelope/"
#define SOAP12_NS "http://www.w3.org/2003/05/soap-envelope"

/* ── SOAP Envelope Building ────────────────────────────────────────────── */

static const char *build_envelope(const char *body_xml,
                                  const char *header_xml,
                                  const char *ns) {
    const char *body = safe_str(body_xml);
    int has_header = !is_empty(header_xml);

    /*
     * Template:
     * <?xml version="1.0" encoding="UTF-8"?>\n
     * <soap:Envelope xmlns:soap="NS">\n
     *   <soap:Header>HEADER</soap:Header>\n    (optional)
     *   <soap:Body>BODY</soap:Body>\n
     * </soap:Envelope>
     */
    size_t ns_len = tk_strlen(ns);
    size_t body_len = tk_strlen(body);
    size_t header_len = has_header ? tk_strlen(header_xml) : 0;

    /* Calculate buffer size with generous padding */
    size_t buf_size = 256 + ns_len + body_len + header_len;
    char *buf = (char *)malloc(buf_size);
    if (!buf) return sealed_empty();

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, buf_size - pos,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<soap:Envelope xmlns:soap=\"%s\">\n", ns);

    if (has_header) {
        pos += (size_t)snprintf(buf + pos, buf_size - pos,
            "  <soap:Header>%s</soap:Header>\n", header_xml);
    }

    /* Last write: buf_size was computed from the three lengths above, so
     * every write fit and pos is the final length. */
    pos += (size_t)snprintf(buf + pos, buf_size - pos,
        "  <soap:Body>%s</soap:Body>\n"
        "</soap:Envelope>", body);

    return seal_take(buf, pos);
}

const char *soap_envelope(const char *body_xml, const char *header_xml) {
    return build_envelope(body_xml, header_xml, SOAP11_NS);
}

const char *soap12_envelope(const char *body_xml, const char *header_xml) {
    return build_envelope(body_xml, header_xml, SOAP12_NS);
}

/* ── SOAP Fault ────────────────────────────────────────────────────────── */

const char *soap_fault(const char *faultcode, const char *faultstring,
                       const char *detail) {
    const char *fc = safe_str(faultcode);
    const char *fs = safe_str(faultstring);
    int has_detail = !is_empty(detail);

    /* 121.16 (PAR-11): detail is escaped like the other two fields.  It
     * went in raw, so a detail carrying request data could close <detail>
     * and inject elements into the fault (ADR-0011: the safe form is the
     * default). */
    size_t n_fc, n_fs, n_dt = 0;
    char *esc_fc = xml_escape(fc, &n_fc);
    char *esc_fs = xml_escape(fs, &n_fs);
    char *esc_dt = has_detail ? xml_escape(detail, &n_dt) : NULL;

    size_t buf_size = 256 + n_fc + n_fs + n_dt;
    char *buf = (esc_fc && esc_fs && (esc_dt || !has_detail))
              ? (char *)malloc(buf_size) : NULL;
    if (!buf) {
        free(esc_fc);
        free(esc_fs);
        free(esc_dt);
        return sealed_empty();
    }

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, buf_size - pos,
        "<soap:Fault>\n"
        "  <faultcode>%s</faultcode>\n"
        "  <faultstring>%s</faultstring>\n", esc_fc, esc_fs);

    if (esc_dt) {
        pos += (size_t)snprintf(buf + pos, buf_size - pos,
            "  <detail>%s</detail>\n", esc_dt);
    }

    pos += (size_t)snprintf(buf + pos, buf_size - pos, "</soap:Fault>");

    free(esc_fc);
    free(esc_fs);
    free(esc_dt);
    return seal_take(buf, pos);
}

/* ── SOAP Extraction ───────────────────────────────────────────────────── */

const char *soap_extract_body(const char *envelope) {
    if (!envelope) return sealed_empty();

    /* Try soap:Body first (namespace-prefixed) */
    char *result = find_tag_content(envelope, "<soap:Body", "</soap:Body>");
    if (result) return result;

    /* Try SOAP-ENV:Body (alternate prefix) */
    result = find_tag_content(envelope, "<SOAP-ENV:Body", "</SOAP-ENV:Body>");
    if (result) return result;

    /* Try Body without prefix */
    result = find_tag_content(envelope, "<Body", "</Body>");
    if (result) return result;

    return sealed_empty();
}

const char *soap_extract_fault(const char *envelope) {
    if (!envelope) return NULL;

    /* Try soap:Fault first */
    const char *start = strstr(envelope, "<soap:Fault");
    const char *end_tag = "</soap:Fault>";
    if (!start) {
        start = strstr(envelope, "<SOAP-ENV:Fault");
        end_tag = "</SOAP-ENV:Fault>";
    }
    if (!start) {
        start = strstr(envelope, "<Fault");
        end_tag = "</Fault>";
    }
    if (!start) return NULL;

    const char *end = strstr(start, end_tag);
    if (!end) return NULL;

    end += tk_strlen(end_tag);
    return tks_dup(start, (size_t)(end - start));
}

/* ── WS-Security Stubs (Story 88.6.5) ─────────────────────────────────── */

#define WSSE_NS "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
#define WSU_NS  "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
#define WSSE_PASSWORD_TEXT "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordText"

const char *soap_wsse_username_token(const char *username,
                                     const char *password) {
    const char *u = safe_str(username);
    const char *p = safe_str(password);

    size_t n_u, n_p;
    char *esc_u = xml_escape(u, &n_u);
    char *esc_p = xml_escape(p, &n_p);

    size_t buf_size = 512 + n_u + n_p;
    char *buf = (esc_u && esc_p) ? (char *)malloc(buf_size) : NULL;
    if (!buf) {
        free(esc_u);
        free(esc_p);
        return sealed_empty();
    }

    int len = snprintf(buf, buf_size,
        "<wsse:Security xmlns:wsse=\"" WSSE_NS "\">\n"
        "  <wsse:UsernameToken>\n"
        "    <wsse:Username>%s</wsse:Username>\n"
        "    <wsse:Password Type=\"" WSSE_PASSWORD_TEXT "\">%s</wsse:Password>\n"
        "  </wsse:UsernameToken>\n"
        "</wsse:Security>",
        esc_u, esc_p);

    free(esc_u);
    free(esc_p);
    return seal_take(buf, len > 0 ? (size_t)len : 0);
}

const char *soap_wsse_timestamp(const char *created, const char *expires) {
    const char *c = safe_str(created);
    const char *e = safe_str(expires);

    size_t n_c, n_e;
    char *esc_c = xml_escape(c, &n_c);
    char *esc_e = xml_escape(e, &n_e);

    size_t buf_size = 512 + n_c + n_e;
    char *buf = (esc_c && esc_e) ? (char *)malloc(buf_size) : NULL;
    if (!buf) {
        free(esc_c);
        free(esc_e);
        return sealed_empty();
    }

    int len = snprintf(buf, buf_size,
        "<wsu:Timestamp xmlns:wsu=\"" WSU_NS "\">\n"
        "  <wsu:Created>%s</wsu:Created>\n"
        "  <wsu:Expires>%s</wsu:Expires>\n"
        "</wsu:Timestamp>",
        esc_c, esc_e);

    free(esc_c);
    free(esc_e);
    return seal_take(buf, len > 0 ? (size_t)len : 0);
}
