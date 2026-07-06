/*
 * ws_glue.c — i64-ABI wrappers for std.ws (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=ws:std.ws` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "ws.h"
#include "capabilities.h"   /* 124.4c: net capability gate */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_ws_connect_w(int64_t url) {
    TK_REQUIRE(TK_CAP_NET);
    if (!url) return 0;
    WsConnResult r = ws_connect((const char *)(intptr_t)url);
    if (r.is_err || !r.conn) return 0;
    return (int64_t)(intptr_t)r.conn;
}

int64_t tk_ws_send_w(int64_t conn, int64_t msg) {
    if (!conn || !msg) return -1;
    WsSendResult r = ws_send((WsConn *)(intptr_t)conn,
                              (const char *)(intptr_t)msg);
    return r.is_err ? -1 : 0;
}

int64_t tk_ws_recv_w(int64_t conn) {
    if (!conn) return 0;
    WsRecvResult r = ws_recv((WsConn *)(intptr_t)conn);
    if (r.is_err || !r.frame) return 0;
    /* Return the payload as a string (for text frames) */
    if (r.frame->opcode == WS_TEXT && r.frame->payload && r.frame->payload_len > 0) {
        char *s = (char *)malloc(r.frame->payload_len + 1);
        if (s) {
            memcpy(s, r.frame->payload, r.frame->payload_len);
            s[r.frame->payload_len] = '\0';
            ws_frame_free(r.frame);
            return (int64_t)(intptr_t)s;
        }
    }
    ws_frame_free(r.frame);
    return 0;
}

int64_t tk_ws_close_w(int64_t conn) {
    if (!conn) return 0;
    ws_close((WsConn *)(intptr_t)conn);
    ws_conn_free((WsConn *)(intptr_t)conn);
    return 0;
}
