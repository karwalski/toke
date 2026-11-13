/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_SOAP_H
#define TK_STDLIB_SOAP_H

/*
 * soap.h — C interface for the std.soap standard library module.
 *
 * Provides SOAP 1.1 and 1.2 envelope building, fault generation,
 * body/fault extraction, and WS-Security header stubs.
 *
 * Pure C, no external dependencies beyond libc.
 * Forward-declares xml.h types; link with xml.c when available.
 *
 * Ownership (127.295, runtime-abi §3.1): every string these functions
 * return is a sealed string from the string heap (tk_strhdr.h).  Never
 * free, write or realloc one -- C callers included; copy it to modify it.
 *
 * Story: 88.6.3
 */

/* ── SOAP Envelope Building ────────────────────────────────────────────── */

/* Build a SOAP 1.1 envelope.
 * body_xml: XML string to place inside <soap:Body>.
 * header_xml: XML string to place inside <soap:Header> (NULL or "" to omit).
 * Returns a sealed XML string (never freed; see above). */
const char *soap_envelope(const char *body_xml, const char *header_xml);

/* Build a SOAP 1.2 envelope.
 * Uses namespace http://www.w3.org/2003/05/soap-envelope.
 * body_xml: XML string to place inside <soap:Body>.
 * header_xml: XML string to place inside <soap:Header> (NULL or "" to omit).
 * Returns a sealed XML string (never freed; see above). */
const char *soap12_envelope(const char *body_xml, const char *header_xml);

/* Build a SOAP 1.1 Fault element.
 * faultcode: e.g. "Server", "Client".
 * faultstring: human-readable fault description.
 * detail: optional detail TEXT, XML-escaped like the other two fields
 *         (NULL or "" to omit <detail>).  121.16: it was embedded raw.
 * Returns a sealed XML string (never freed; see above). */
const char *soap_fault(const char *faultcode, const char *faultstring,
                       const char *detail);

/* ── SOAP Extraction ───────────────────────────────────────────────────── */

/* Extract the inner content of <soap:Body> from a SOAP envelope.
 * Returns a sealed string with the body content, or a sealed "" on failure. */
const char *soap_extract_body(const char *envelope);

/* Extract the <soap:Fault> element from a SOAP envelope.
 * Returns a sealed string with the fault XML, or NULL if no fault
 * is present. */
const char *soap_extract_fault(const char *envelope);

/* ── WS-Security Stubs (Story 88.6.5) ─────────────────────────────────── */

/* Build a WS-Security UsernameToken header block.
 * Returns a sealed XML string (never freed; see above). */
const char *soap_wsse_username_token(const char *username,
                                     const char *password);

/* Build a WS-Security Timestamp header block.
 * created/expires: ISO 8601 timestamps (e.g. "2024-01-01T00:00:00Z").
 * Returns a sealed XML string (never freed; see above). */
const char *soap_wsse_timestamp(const char *created, const char *expires);

#endif /* TK_STDLIB_SOAP_H */
