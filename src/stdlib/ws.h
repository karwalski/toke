#ifndef TK_STDLIB_WS_H
#define TK_STDLIB_WS_H

/*
 * ws.h — C interface for the std.ws standard library module.
 *
 * Implements WebSocket framing per RFC 6455.
 *
 * Type mappings:
 *   Str     = const char *  (null-terminated UTF-8)
 *   payload = uint8_t * with explicit length field
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 * SHA-1 for the handshake accept-key is implemented inline per RFC 3174.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own returned pointers; use ws_frame_free /
 * ws_encode_result_free to release.
 *
 * Story: 15.1.1
 */

#include <stddef.h>
#include <stdint.h>
#include "str.h"

/* WebSocket opcodes (RFC 6455 §5.2) */
typedef enum {
    WS_TEXT   = 0x1,
    WS_BINARY = 0x2,
    WS_CLOSE  = 0x8,
    WS_PING   = 0x9,
    WS_PONG   = 0xA
} WsOpcode;

/* A decoded WebSocket frame. */
typedef struct {
    WsOpcode  opcode;
    uint8_t  *payload;
    uint64_t  payload_len;
    int       is_final;   /* FIN bit */
} WsFrame;

/* Result of ws_decode_frame. */
typedef struct {
    WsFrame    *frame;    /* NULL if error */
    int         is_err;
    const char *err_msg;
} WsFrameResult;

/* Result of ws_encode_frame. */
typedef struct {
    uint8_t    *data;
    uint64_t    len;
    int         is_err;
    const char *err_msg;
} WsEncodeResult;

/* Frame encoding (client→server: masked; server→client: unmasked).
 * Caller owns result.data; use ws_encode_result_free to release. */
WsEncodeResult ws_encode_frame(WsOpcode opcode, const uint8_t *payload,
                               uint64_t plen, int mask);

/* Frame decoding from raw bytes.
 * On success, *consumed_out is set to the total number of bytes consumed.
 * Caller owns result.frame; use ws_frame_free to release. */
WsFrameResult  ws_decode_frame(const uint8_t *buf, uint64_t buflen,
                               uint64_t *consumed_out);

/* Compute the Sec-WebSocket-Accept value for a given client key.
 * Returns a heap-allocated null-terminated base64 string.
 * Caller owns the returned pointer (free with free()). */
const char    *ws_accept_key(const char *client_key);

/* Return 1 if the given HTTP header block contains an Upgrade: websocket
 * request (case-insensitive match), 0 otherwise. */
int            ws_is_upgrade_request(const char *http_headers);

/* Release resources. Safe to call with NULL. */
void           ws_frame_free(WsFrame *f);
void           ws_encode_result_free(WsEncodeResult *r);

/* -----------------------------------------------------------------------
 * High-level connection API (Story 35.1.4)
 *
 * Maps to the std.ws .tki contract: ws.connect, ws.send, ws.sendbytes,
 * ws.recv, ws.close, ws.broadcast.
 * Built on top of the low-level frame encode/decode functions above.
 * ----------------------------------------------------------------------- */

/* Connection handle — wraps a TCP socket with WebSocket state. */
typedef struct {
    int      fd;        /* underlying TCP socket descriptor        */
    int      mask;      /* 1 = client (mask outgoing frames)       */
    int      closed;    /* 1 = close frame sent or received        */
} WsConn;

/* Result of ws_connect. */
typedef struct {
    WsConn     *conn;     /* NULL on error */
    int         is_err;
    const char *err_msg;
} WsConnResult;

/* Result of ws_recv. */
typedef struct {
    WsFrame    *frame;    /* NULL on error */
    int         is_err;
    const char *err_msg;
} WsRecvResult;

/* Result of ws_send / ws_sendbytes. */
typedef struct {
    int         is_err;
    const char *err_msg;
} WsSendResult;

/* Connect to a WebSocket server (ws:// URL).
 * Performs TCP connect + HTTP Upgrade handshake.
 * Caller owns the returned WsConn; release with ws_close + ws_conn_free. */
WsConnResult   ws_connect(const char *url);

/* Send a UTF-8 text message on an open connection. */
WsSendResult   ws_send(WsConn *conn, const char *text);

/* Send a binary message on an open connection. */
WsSendResult   ws_sendbytes(WsConn *conn, const uint8_t *data, uint64_t len);

/* Receive the next complete message (blocks until a full frame arrives). */
WsRecvResult   ws_recv(WsConn *conn);

/* Send a close frame and shut down the connection. Safe to call twice. */
void           ws_close(WsConn *conn);

/* Send the same text message to every connection in the array. */
void           ws_broadcast(WsConn **conns, uint64_t count, const char *text);

/* Free a WsConn struct. Call ws_close first if still open. */
void           ws_conn_free(WsConn *conn);

/* -----------------------------------------------------------------------
 * Close frame helpers and upgrade utilities (Story 34.6.1)
 * ----------------------------------------------------------------------- */

/* Parsed WebSocket close frame (RFC 6455 §5.5.1). */
typedef struct {
    uint16_t    code;   /* status code, e.g. 1000 */
    const char *reason; /* UTF-8 reason string (points into caller-managed buf) */
} WsCloseFrame;

/* Parse a close frame payload.
 * If payload.len == 0: code=1000, reason="".
 * If payload.len >= 2: code = ntohs(first 2 bytes), reason = remaining bytes
 *   interpreted as a null-terminated string (caller must ensure payload.data
 *   has room for a NUL terminator, or use the returned reason within the
 *   lifetime of payload.data).
 * The returned reason pointer aliases payload.data. */
WsCloseFrame ws_handle_close_frame(ByteArray payload);

/* Build a close frame payload: 2-byte big-endian status code + reason bytes.
 * Returns a heap-allocated ByteArray; caller owns .data (free with free()). */
ByteArray    ws_build_close_frame(uint16_t code, const char *reason);

/* Headers extracted from a WebSocket upgrade request. */
typedef struct {
    const char *sec_key;    /* Sec-WebSocket-Key value, or NULL if absent */
    const char *protocol;   /* Sec-WebSocket-Protocol value, or NULL */
    const char *extensions; /* Sec-WebSocket-Extensions value, or NULL */
} WsUpgradeHeaders;

/* Scan an array of header name/value pairs (nhdrs entries) for the three
 * WebSocket upgrade headers.  Header name comparison is case-insensitive.
 * The returned string pointers alias the supplied hvalues array. */
WsUpgradeHeaders ws_parse_upgrade_headers(const char *const *hnames,
                                          const char *const *hvalues,
                                          uint64_t nhdrs);

/* Build the HTTP/1.1 101 Switching Protocols upgrade response string.
 * protocols may be NULL (no Sec-WebSocket-Protocol header added).
 * Returns a heap-allocated null-terminated string; caller owns it. */
const char *ws_build_upgrade_response(const char *accept_key,
                                      const char *protocols);

/* Validate a UTF-8 byte sequence of length len (not null-terminated).
 * Returns 1 if valid UTF-8 per RFC 6455 §3.4, 0 otherwise.
 * Rejects overlong encodings, surrogates, and code points above U+10FFFF. */
int ws_validate_utf8(const uint8_t *payload, size_t len);

#endif /* TK_STDLIB_WS_H */
