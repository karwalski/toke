/*
 * tk_http_field.h — whether a response header field may be written as
 * "name: value\r\n" (stories 121.90, 121.92).
 *
 * One rule for every HTTP/1.1 response writer in the stdlib: router.c's
 * router_send_response (121.90) and http.c's handler-header writer, plain
 * and TLS (121.92).  A field whose name is not an RFC 9110 token
 * (sec. 5.1), or whose value holds a control byte other than HTAB
 * (sec. 5.5), is DROPPED whole by the writer -- never repaired: CR or LF
 * there is a response-splitting attempt or a bug, and stripping the bytes
 * would still send a value nobody wrote.
 *
 * Header-only and static, like tk_gzip_type.h, so every stdlib translation
 * unit that includes it links without a new object in any link line.
 */
#ifndef TK_HTTP_FIELD_H
#define TK_HTTP_FIELD_H

#if defined(__GNUC__) || defined(__clang__)
#define TK_HTTP_FIELD_UNUSED __attribute__((unused))
#else
#define TK_HTTP_FIELD_UNUSED
#endif

/* 1 when n is a non-empty RFC 9110 token: no CR, LF, colon, space or other
 * separator can end the field name early. */
static TK_HTTP_FIELD_UNUSED int tk_http_field_name_ok(const char *n)
{
    if (!n || !*n) return 0;
    for (const unsigned char *p = (const unsigned char *)n; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            continue;
        switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            continue;
        default:
            return 0;
        }
    }
    return 1;
}

/* 1 when v holds no control byte but HTAB (and no DEL): no CR or LF to
 * start a forged header line or end the header block. */
static TK_HTTP_FIELD_UNUSED int tk_http_field_value_ok(const char *v)
{
    if (!v) return 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return 0;
    return 1;
}

#endif /* TK_HTTP_FIELD_H */
