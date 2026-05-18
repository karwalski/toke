/*
 * soap.c — SOAP 1.1/1.2 envelope builder, fault generator, extractor,
 *           and WS-Security header stubs for the std.soap module.
 *
 * Pure C implementation. Uses snprintf + malloc for string building.
 * No external dependencies beyond libc.
 *
 * Stories: 88.6.3, 88.6.5
 */

#include "soap.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Safe strlen that treats NULL as "" */
static size_t safe_len(const char *s) {
    return s ? strlen(s) : 0;
}

/* Safe pointer that treats NULL as "" */
static const char *safe_str(const char *s) {
    return s ? s : "";
}

/* Return 1 if s is NULL or empty */
static int is_empty(const char *s) {
    return !s || s[0] == '\0';
}

/*
 * xml_escape — escape &, <, >, ", ' for safe XML embedding.
 * Returns a heap-allocated string. Caller owns the result.
 */
static char *xml_escape(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    /* Worst case: every char becomes &quot; (6 chars) */
    size_t cap = len * 6 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return strdup("");
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
    return out;
}

/*
 * find_tag_content — find content between <prefix...> and </tag>.
 * Searches for the opening tag prefix and closing tag, copies the
 * content between them into a heap-allocated string.
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

    size_t content_len = (size_t)(end - content_start);
    char *result = (char *)malloc(content_len + 1);
    if (!result) return NULL;
    memcpy(result, content_start, content_len);
    result[content_len] = '\0';
    return result;
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
    size_t ns_len = strlen(ns);
    size_t body_len = strlen(body);
    size_t header_len = has_header ? strlen(header_xml) : 0;

    /* Calculate buffer size with generous padding */
    size_t buf_size = 256 + ns_len + body_len + header_len;
    char *buf = (char *)malloc(buf_size);
    if (!buf) return strdup("");

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, buf_size - pos,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<soap:Envelope xmlns:soap=\"%s\">\n", ns);

    if (has_header) {
        pos += (size_t)snprintf(buf + pos, buf_size - pos,
            "  <soap:Header>%s</soap:Header>\n", header_xml);
    }

    pos += (size_t)snprintf(buf + pos, buf_size - pos,
        "  <soap:Body>%s</soap:Body>\n"
        "</soap:Envelope>", body);

    return buf;
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

    char *esc_fc = xml_escape(fc);
    char *esc_fs = xml_escape(fs);

    size_t buf_size = 256 + strlen(esc_fc) + strlen(esc_fs)
                    + (has_detail ? strlen(detail) : 0);
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        free(esc_fc);
        free(esc_fs);
        return strdup("");
    }

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, buf_size - pos,
        "<soap:Fault>\n"
        "  <faultcode>%s</faultcode>\n"
        "  <faultstring>%s</faultstring>\n", esc_fc, esc_fs);

    if (has_detail) {
        pos += (size_t)snprintf(buf + pos, buf_size - pos,
            "  <detail>%s</detail>\n", detail);
    }

    snprintf(buf + pos, buf_size - pos, "</soap:Fault>");

    free(esc_fc);
    free(esc_fs);
    return buf;
}

/* ── SOAP Extraction ───────────────────────────────────────────────────── */

const char *soap_extract_body(const char *envelope) {
    if (!envelope) return strdup("");

    /* Try soap:Body first (namespace-prefixed) */
    char *result = find_tag_content(envelope, "<soap:Body", "</soap:Body>");
    if (result) return result;

    /* Try SOAP-ENV:Body (alternate prefix) */
    result = find_tag_content(envelope, "<SOAP-ENV:Body", "</SOAP-ENV:Body>");
    if (result) return result;

    /* Try Body without prefix */
    result = find_tag_content(envelope, "<Body", "</Body>");
    if (result) return result;

    return strdup("");
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

    end += strlen(end_tag);
    size_t len = (size_t)(end - start);
    char *result = (char *)malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

/* ── WS-Security Stubs (Story 88.6.5) ─────────────────────────────────── */

#define WSSE_NS "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
#define WSU_NS  "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
#define WSSE_PASSWORD_TEXT "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordText"

const char *soap_wsse_username_token(const char *username,
                                     const char *password) {
    const char *u = safe_str(username);
    const char *p = safe_str(password);

    char *esc_u = xml_escape(u);
    char *esc_p = xml_escape(p);

    size_t buf_size = 512 + strlen(esc_u) + strlen(esc_p);
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        free(esc_u);
        free(esc_p);
        return strdup("");
    }

    snprintf(buf, buf_size,
        "<wsse:Security xmlns:wsse=\"" WSSE_NS "\">\n"
        "  <wsse:UsernameToken>\n"
        "    <wsse:Username>%s</wsse:Username>\n"
        "    <wsse:Password Type=\"" WSSE_PASSWORD_TEXT "\">%s</wsse:Password>\n"
        "  </wsse:UsernameToken>\n"
        "</wsse:Security>",
        esc_u, esc_p);

    free(esc_u);
    free(esc_p);
    return buf;
}

const char *soap_wsse_timestamp(const char *created, const char *expires) {
    const char *c = safe_str(created);
    const char *e = safe_str(expires);

    char *esc_c = xml_escape(c);
    char *esc_e = xml_escape(e);

    size_t buf_size = 512 + strlen(esc_c) + strlen(esc_e);
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        free(esc_c);
        free(esc_e);
        return strdup("");
    }

    snprintf(buf, buf_size,
        "<wsu:Timestamp xmlns:wsu=\"" WSU_NS "\">\n"
        "  <wsu:Created>%s</wsu:Created>\n"
        "  <wsu:Expires>%s</wsu:Expires>\n"
        "</wsu:Timestamp>",
        esc_c, esc_e);

    free(esc_c);
    free(esc_e);
    return buf;
}
