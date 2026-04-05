# std.ws

WebSocket client: connect, send text or binary frames, receive messages, broadcast, and close.

## Types

```
type wsconn { id: u64, ready: bool }
type wsmsg  { payload: [byte], fin: bool, opcode: u64 }
type wserr  { msg: str }
```

## Functions

```
f=connect(url:str):wsconn!wserr
f=send(conn:wsconn;text:str):void!wserr
f=sendbytes(conn:wsconn;data:[byte]):void!wserr
f=recv(conn:wsconn):wsmsg!wserr
f=close(conn:wsconn):void
f=broadcast(conns:[wsconn];text:str):void
```

## Semantics

- `connect` performs the WebSocket upgrade handshake to the given URL. Returns `wserr` on DNS failure, connection refused, or failed upgrade.
- `send` transmits a text frame (opcode 1); `sendbytes` transmits a binary frame (opcode 2).
- `recv` blocks until a complete message arrives. Returns `wserr` on connection loss.
- `close` sends a close frame and releases the connection. Calling `send`/`recv` after `close` is undefined.
- `broadcast` sends the same text frame to every connection in the list. Failures on individual connections are silently ignored.
- `wsmsg.opcode` follows RFC 6455: 1 = text, 2 = binary. `fin` indicates the final fragment.

## Dependencies

None (uses runtime TCP/TLS directly).
