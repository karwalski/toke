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

#include <stdint.h>

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

#endif /* TK_STDLIB_WS_H */
