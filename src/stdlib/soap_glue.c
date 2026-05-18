/*
 * soap_glue.c — i64-ABI wrappers for std.soap module.
 *
 * Provides the glue between the toke i64 calling convention and the
 * C soap.h API. Each wrapper converts i64 pointers to const char*,
 * calls the underlying C function, and returns the result as i64.
 *
 * Stories: 88.6.4, 88.6.5
 */

#include "soap.h"
#include <stdint.h>
#include <stdlib.h>

/* ── SOAP Envelope Glue (88.6.4) ──────────────────────────────────────── */

/* soap.envelope(body, header) → str */
int64_t tk_soap_envelope_w(int64_t body, int64_t header) {
    const char *b = body   ? (const char *)(intptr_t)body   : "";
    const char *h = header ? (const char *)(intptr_t)header : "";
    const char *result = soap_envelope(b, h);
    return (int64_t)(intptr_t)result;
}

/* soap.envelope12(body, header) → str */
int64_t tk_soap12_envelope_w(int64_t body, int64_t header) {
    const char *b = body   ? (const char *)(intptr_t)body   : "";
    const char *h = header ? (const char *)(intptr_t)header : "";
    const char *result = soap12_envelope(b, h);
    return (int64_t)(intptr_t)result;
}

/* soap.fault(code, string, detail) → str */
int64_t tk_soap_fault_w(int64_t code, int64_t string, int64_t detail) {
    const char *c = code   ? (const char *)(intptr_t)code   : "";
    const char *s = string ? (const char *)(intptr_t)string : "";
    const char *d = detail ? (const char *)(intptr_t)detail : "";
    const char *result = soap_fault(c, s, d);
    return (int64_t)(intptr_t)result;
}

/* soap.extractbody(envelope) → str */
int64_t tk_soap_extractbody_w(int64_t envelope) {
    if (!envelope) return (int64_t)(intptr_t)"";
    const char *e = (const char *)(intptr_t)envelope;
    const char *result = soap_extract_body(e);
    return (int64_t)(intptr_t)result;
}

/* soap.extractfault(envelope) → str (0 if no fault) */
int64_t tk_soap_extractfault_w(int64_t envelope) {
    if (!envelope) return 0;
    const char *e = (const char *)(intptr_t)envelope;
    const char *result = soap_extract_fault(e);
    return (int64_t)(intptr_t)result;
}

/* ── WS-Security Glue (88.6.5) ────────────────────────────────────────── */

/* soap.wsseusernametoken(user, pass) → str */
int64_t tk_soap_wsseusernametoken_w(int64_t user, int64_t pass) {
    const char *u = user ? (const char *)(intptr_t)user : "";
    const char *p = pass ? (const char *)(intptr_t)pass : "";
    const char *result = soap_wsse_username_token(u, p);
    return (int64_t)(intptr_t)result;
}

/* soap.wssetimestamp(created, expires) → str */
int64_t tk_soap_wssetimestamp_w(int64_t created, int64_t expires) {
    const char *c = created ? (const char *)(intptr_t)created : "";
    const char *e = expires ? (const char *)(intptr_t)expires : "";
    const char *result = soap_wsse_timestamp(c, e);
    return (int64_t)(intptr_t)result;
}
