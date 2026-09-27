/*
 * ws_glue.c — i64-ABI wrappers for std.ws (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=ws:std.ws` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "ws.h"
#include "capabilities.h"   /* 124.4c: net capability gate */
#include "tk_array.h"    /* 127.145: record/array backing blocks */
#include "bytes_rt.h"    /* 127.145: [byte] marshalling */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tk_runtime.h"   /* 127.238: tk_current_error + tk_err_box */

/* 127.238: the error slot (runtime-abi.md §7.2).  These `T!wserr` wrappers
 * returned a bare 0 on failure, so only the value sentinel saw it and the
 * $err arm bound nil (`e.msg` was RT005).  The failure value is unchanged
 * (0; every T here is a handle or pointer); every return now goes through
 * ws_slot, which on 0 also stores a wserr record {msg; …} — the other
 * fields 0 — and on anything else clears the slot, so the wrapper is a slot
 * wrapper (llvm.c derives them from the .tki). */
static int64_t ws_slot(int64_t v, const char *msg) {
    if (v) { tk_current_error = 0; return v; }
    int64_t *box = (int64_t *)tk_err_box(1 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msg;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

static const char tk_ws_connect_fail[] =
    "ws.connect: the connection could not be opened";
int64_t tk_ws_connect_w(int64_t url) {
    TK_REQUIRE(TK_CAP_NET);
    if (!url) return ws_slot(0, tk_ws_connect_fail);
    WsConnResult r = ws_connect((const char *)(intptr_t)url);
    if (r.is_err || !r.conn) return ws_slot(0, tk_ws_connect_fail);
    return ws_slot((int64_t)(intptr_t)r.conn, tk_ws_connect_fail);
}

/*
 * 127.112 — the handle a toke `$wsconn` carries is an address, and until this
 * story nothing checked that the address was one WE handed out.  The
 * `docs/stdlib/ws.md` example fabricated a `$wsconn{id:0;ready:false}` as the
 * `$err` arm of ws.connect; that zeroed block read as an OPEN connection on
 * fd 0, so ws.send wrote a real WebSocket frame to the caller's terminal and
 * reported success.  ws_conn_valid() is the guard; it lives in ws.c so every
 * caller of the C API gets it, and the wrappers below fail closed.
 */
static WsConn *ws_handle(int64_t h) {
    WsConn *c = (WsConn *)(intptr_t)h;
    return ws_conn_valid(c) ? c : NULL;
}

int64_t tk_ws_send_w(int64_t conn, int64_t msg) {
    if (!msg) return -1;
    WsConn *c = ws_handle(conn);
    if (!c) return -1;
    WsSendResult r = ws_send(c, (const char *)(intptr_t)msg);
    return r.is_err ? -1 : 0;
}

/*
 * 127.150 — ws.sendbytes(conn; data: @($byte)) : void!$wserr.
 *
 * Declared in stdlib/ws.tki and documented in docs/stdlib/ws.md with no
 * caveat, while no _w wrapper existed, so any program naming it died at ld
 * on a bare _tk_ws_sendbytes_w. The C core has had ws_sendbytes() (a WS_BINARY
 * frame through the same ws_encode_frame / ws_send_all path ws_send uses)
 * all along; this is the missing door. Same error convention as ws.send:
 * 0 is $ok, -1 is $err. A [byte] is an i64 array of byte values (bytes_rt.h),
 * so it is unpacked to a contiguous buffer first — an embedded 0x00 is data.
 */
int64_t tk_ws_sendbytes_w(int64_t conn, int64_t data) {
    WsConn *c = ws_handle(conn);
    if (!c) return -1;
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    if (!buf) return -1;
    WsSendResult r = ws_sendbytes(c, buf, n);
    free(buf);
    return r.is_err ? -1 : 0;
}

/*
 * 127.145 — ws.recv is declared `$wsconn -> $wsmsg!$wserr` and returned a
 * bare `char *`.
 *
 * THE DEFECT.  This wrapper copied the frame payload into a NUL-terminated
 * string and handed the string pointer back as the `$ok` arm.  A toke
 * `$wsmsg` is a three-slot record, so `msg.payload` GEP'd slot 0 of a char
 * buffer and loaded it as a pointer: SIGBUS, exit 138, verified against a
 * live WebSocket server with a clean handshake and "hello" delivered.  The
 * Combined Example in docs/stdlib/ws.md did exactly that, so the documented
 * way to use the module crashed.
 *
 * Same family as 127.112 and pointing the other way: there a fabricated toke
 * struct was read as a C handle, here a C string was handed out as a toke
 * struct.  Both are the interface and the runtime disagreeing about what
 * crosses the boundary, and in both directions the answer is to make the
 * runtime keep the interface's promise.
 *
 * WHY A REAL $wsmsg AND NOT `str!$wserr`.  Narrowing the declared type would
 * have matched the defect instead of fixing it, and it would have made three
 * losses permanent.  `fin` and `opcode` are protocol facts the interface
 * already promises and a fragmented or control-frame-aware client needs.
 * Worse, the old code returned 0 — indistinguishable from the `$err` arm —
 * for EVERY non-text frame, so `ws.sendbytes` existed with no way to receive
 * what it sends, and an empty text frame was reported as a failure.  The ABI
 * to do it properly was already in the tree and already exercised:
 * `time.toparts` builds a record as an i64-slot block (one slot per field, in
 * declaration order) and `tk_bytes_pack` builds a `[byte]` as an i64 array.
 *
 * Slot order is stdlib/ws.tki's field order: payload, fin, opcode.
 */
static const char tk_ws_recv_fail[] =
    "ws.recv: no message (the connection is closed, or the frame could not be read)";
int64_t tk_ws_recv_w(int64_t conn) {
    WsConn *c = ws_handle(conn);
    if (!c) return ws_slot(0, tk_ws_recv_fail);
    WsRecvResult r = ws_recv(c);
    if (r.is_err || !r.frame) return ws_slot(0, tk_ws_recv_fail);

    /* Binary-safe: a [byte] is an i64 array, so an embedded NUL is data.
     * A zero-length payload still yields a real, empty array handle — it is
     * an empty message, not an error. */
    int64_t payload = tk_bytes_pack(r.frame->payload,
                                    r.frame->payload ? r.frame->payload_len : 0);
    if (!payload) { ws_frame_free(r.frame); return ws_slot(0, tk_ws_recv_fail); }

    int64_t msg = tk_arr_alloc(3, 3);
    if (!msg) {
        /* The payload block was never published, so release it rather than
         * leaving it behind on the OOM path. */
        free((int64_t *)(intptr_t)payload - TK_ARR_HDR);
        ws_frame_free(r.frame);
        return ws_slot(0, tk_ws_recv_fail);
    }
    int64_t *slot = (int64_t *)(intptr_t)msg;
    slot[0] = payload;                            /* payload : @($byte) */
    slot[1] = r.frame->is_final ? 1 : 0;          /* fin     : bool     */
    slot[2] = (int64_t)r.frame->opcode;           /* opcode  : u64      */

    ws_frame_free(r.frame);
    return ws_slot(msg, tk_ws_recv_fail);
}

int64_t tk_ws_close_w(int64_t conn) {
    /* Without the validation this close()d fd 0 and then free()d a pointer
     * the toke runtime owns — a documented example reached both.
     *
     * 135.20: it then free()d the WsConn itself (ws_conn_free), but the toke
     * program still holds the handle, and the documented pattern -- close,
     * then a send that must take the $err arm -- made ws_conn_valid() read
     * conn->magic from freed memory (ASan heap-use-after-free, C107).  Freed
     * memory cannot be validated: its contents are whatever the allocator
     * or the next allocation left there, including a live WsConn's magic.
     * So close leaves a CLOSED handle behind (fd -1, closed 1) that every
     * later call rejects by its state, and does not free it.  The cost is
     * the 16-byte struct per closed connection, for the program's lifetime;
     * toke has no destructor that could free it safely. */
    WsConn *c = ws_handle(conn);
    if (!c) return 0;
    ws_close(c);
    return 0;
}

/*
 * Story 136.32 — ws.broadcast(conns: @($wsconn); text: $str) : void.
 *
 * ws_broadcast() has been in ws.c and declared in ws.h since the module
 * shipped and stdlib/ws.tki exports it; the wrapper was missing, so the
 * multi-connection example on docs/stdlib/ws.md failed at link. A toke
 * `@($wsconn)` is an array handle whose slots are the same connection
 * handles tk_ws_connect_w returns, so the slot block is already the
 * `WsConn **` ws_broadcast wants — but a slot can be 0 when a connect failed
 * and the caller took the $err arm, so nulls are compacted out first rather
 * than handed to ws_send.
 */
int64_t tk_ws_broadcast_w(int64_t conns, int64_t text) {
    if (!conns || !text) return 0;
    int64_t n = tk_arr_len(conns);
    if (n <= 0) return 0;
    int64_t *slots = (int64_t *)(intptr_t)conns;
    WsConn **live = (WsConn **)malloc((size_t)n * sizeof(WsConn *));
    if (!live) return 0;
    uint64_t count = 0;
    for (int64_t i = 0; i < n; i++) {
        WsConn *c = ws_handle(slots[i]);   /* 127.112: not just non-NULL */
        if (c) live[count++] = c;
    }
    if (count) ws_broadcast(live, count, (const char *)(intptr_t)text);
    free(live);
    return 0;
}
