---
title: std.tls
slug: tls
section: reference/stdlib
order: 50
---

**Status: Implemented** -- C runtime backing over OpenSSL, reachable from toke since story 136.44. Proved on the wire by conformance test `C022_tls_is_not_a_facade.sh`, which negotiates a real TLS 1.3 session against Python's `ssl` module and asserts mutual auth fails closed.

`std.tls` builds encrypted peer-to-peer and client-server connections directly: key and certificate generation, server listening, client connecting, I/O, certificate pinning, and a human-readable pairing code for out-of-band verification. It is independent of `std.http` -- for HTTPS, use `std.http`.

Every connection is **TLS 1.3 only**. There is no fallback to 1.2 or earlier: the OpenSSL context sets `TLS1_3_VERSION` as both the minimum and the maximum, so a downgrade is not a policy you can misconfigure, it is absent.

Certificates and keys are PEM strings throughout, which makes them easy to persist and inspect. Treat the key half as a secret; `std.securemem` is the place to hold one in memory.

## It was a façade, and that shaped the interface

Before 136.44 none of this was reachable. `src/stdlib/tls.c` was ~910 lines of working TLS 1.3 -- the version floor, `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`, peer pinning, an X25519MLKEM768 hybrid key exchange -- linked against `-lssl -lcrypto`, and every wrapper in `tls_glue.c` was `(void)arg; return 0;`. `tls.listen` returned before it bound a socket. `tls.connect` and `tls.listen` resolved to symbols that did not exist. `peercert`, `fingerprint` and `pairingcode` had no wrapper at all.

That history explains two things about the interface below that otherwise look redundant.

**Build a configuration with the constructors, not a struct literal.** `tls.tlsconfig`, `tls.pinconfig` and `tls.mutualconfig` are how you build a config; `tls.certof` and `tls.keyof` are how you read a keypair. 136.44 shipped them for a reason: until story 136.46 every field on `tlsconfig` and `tlskeypair` was spelled with an underscore (`cert_pem`, `key_pem`, `peer_cert_pem`, `require_mutual`), and toke's default 59-character profile excludes `_`, so `$tlsconfig{cert_pem: ...}` could not be lexed at all. 136.46 renamed the fields to `certpem`, `keypem`, `peercertpem` and `requiremutual`, and `make check-tki-names` now rejects any interface identifier the default profile cannot express. The constructors remain the documented route: they are what the conformance test uses, and mutual auth should be a named intent rather than a boolean somebody forgets to set.

**The handles are opaque i64.** A `tlsconn` or `tlskeypair` arrives in toke as an integer handle; `0` is the failure and none sentinel. Test with `==0`, and write callbacks as `f=name(conn:i64):i64`.

## Types

### tlsconfig

Configuration for either end of a connection. Treat it as opaque: build it with `tls.tlsconfig`, `tls.pinconfig` or `tls.mutualconfig`, and release it with `tls.freeconfig`.

| Field | Type | Meaning |
|---|---|---|
| `certpem` | `str` | this endpoint's certificate (empty for an anonymous client) |
| `keypem` | `str` | the matching private key |
| `peercertpem` | `str` | certificate to pin the peer against (empty = no pinning) |
| `requiremutual` | `bool` | demand a certificate from the peer |

These four were `cert_pem`, `key_pem`, `peer_cert_pem` and `require_mutual` until story 136.46; no toke program could name them, so there are no callers to migrate.

The fields read what the table says (135.30), and the connection calls read a configuration through them, so a `$tlsconfig{...}` literal works with `tls.connect` and `tls.listen`. `tls.freeconfig` frees only a configuration one of the constructors made; on a literal it returns `false` and frees nothing.

### tlsconn

A handle to an established connection. `0` means no connection. Pass it to `tls.read`, `tls.write`, `tls.close`, `tls.protocol`, `tls.group`, `tls.peercert` and `tls.pairingcode`. Its one field, `id` (`str`), is the connection's identifier.

### tlskeypair

A freshly generated certificate (`certpem`) and its private key (`keypem`). Read the two halves as fields or with `tls.certof` and `tls.keyof`, and release it with `tls.freekeypair` (which frees only a keypair the generators made).

### Errors

The two key-generation calls are the module's only fallible calls, and their error is a `str`: the reason generation failed, as the TLS core reports it (for example `unsupported key_alg (want ecdsa-p384 or ml-dsa-65)`, `common_name must not be empty`, or an OpenSSL reason). `$err:e` binds that message. On failure the returned value is also `0`, so a plain `kp==0` check still works; use `mt` when you want the reason.

Connection failures are not errors in this sense: `tls.connect`, `tls.read` and `tls.peercert` return an optional that is `0` when there is nothing, and `tls.listen`, `tls.write` and `tls.close` return `bool`.

Until story 127.104 this page and `tls.tki` declared a `TlsErr` sum with four variants (`CertErr`, `ConnErr`, `PinErr`, `IoErr`). No function returned three of them, the fourth was never populated (the message was discarded), and the variant names could not be written in the default syntax, so there are no callers to migrate.

## Key and certificate generation

### tls.genselfsigned(commonname: str; validdays: i32): tlskeypair!str

Generates a P-384 EC keypair and a self-signed X.509 certificate with `commonname` as both subject and issuer CN, valid for `validdays` days from now. On failure the error is the reason (see [Errors](#errors)) and the value is `0`.

### tls.genselfsignedalg(commonname: str; validdays: i32; keyalg: str): tlskeypair!str

As `tls.genselfsigned`, but chooses the certificate's signing key:

| `keyalg` | Key |
|---|---|
| `""` or `"ecdsa-p384"` | Classical P-384 ECDSA — the default, broad interoperability |
| `"ml-dsa-65"` | Post-quantum ML-DSA-65 (FIPS 204). Requires OpenSSL 3.5 or later |

**It does not fall back.** An algorithm name it does not recognise is refused, and an ML-DSA keygen that fails on an OpenSSL without ML-DSA support is refused — neither quietly returns a P-384 certificate. A caller asking for ML-DSA is asking because it matters to them, so a classical certificate handed back under that name would be the worst available answer. On failure the error is the reason and the value is `0`:

```toke
m=tlsalg;
i=tls:std.tls;
i=io:std.io;

f=main():i64{
  let why=mt tls.genselfsignedalg("pq-host";30;"ml-dsa-65"){$ok:kp "";$err:e e};
  if(why==""){ io.println("ml-dsa-65 certificate generated"); <0 };
  io.println(why);
  <1
};
```

ML-DSA certificates are opt-in for a reason: they will not verify against a classical-only peer. Use them where you control both ends.

### tls.certof(kp: tlskeypair): str

The PEM-encoded certificate from a keypair.

### tls.keyof(kp: tlskeypair): str

The PEM-encoded private key from a keypair. This is secret material.

### tls.freekeypair(kp: tlskeypair): bool

Releases the keypair and zeroes the key. The handle is dangling afterwards.

```toke
m=tlskeygen;
i=tls:std.tls;
i=file:std.file;
i=io:std.io;

f=emit(cn:str;certp:str;keyp:str):i64{
  let kp=tls.genselfsigned(cn;365);
  if(kp==0){ io.println("keygen failed"); <1 };
  let a=mt file.write(certp;tls.certof(kp)){$ok:v v;$err:e false};
  let b=mt file.write(keyp;tls.keyof(kp)){$ok:v v;$err:e false};
  io.println(tls.fingerprint(tls.certof(kp)));
  tls.freekeypair(kp);
  if(a==false){ <1 };
  if(b==false){ <1 };
  <0
};

f=main():i64{
  <emit("toke-demo-server";"/tmp/toke-tls-s.crt";"/tmp/toke-tls-s.key")
};
```

## Building a configuration

### tls.tlsconfig(certpem: str; keypem: str): tlsconfig

A plain endpoint configuration: present this certificate and key, pin nothing, require nothing of the peer.

**On a client this authenticates nothing.** `tls.connect` with a `tlsconfig` accepts whatever certificate the server presents: there is no CA-chain check and no hostname check, and `std.tls` has no route to either -- it never loads a system trust store. The connection is encrypted but not authenticated, so an on-path attacker can present any certificate and be accepted. Pin the server with `tls.pinconfig`, or verify out of band with `tls.pairingcode`, before trusting who is on the other end. (Story 121.19, finding TLS-02; a CA-verified client configuration needs a spec amendment and is not implemented.)

### tls.pinconfig(certpem: str; keypem: str; peercertpem: str): tlsconfig

As above, plus: accept the peer **only** if it presents exactly `peercertpem`. A client may pass `""` for its own certificate and key and still pin the server.

### tls.mutualconfig(certpem: str; keypem: str; peercertpem: str): tlsconfig

As `pinconfig`, and additionally demands a certificate from the peer. A peer that presents none never reaches your handler -- the handshake fails first. That is the fail-closed behaviour `C022` witnesses with an independent TLS implementation.

### tls.freeconfig(cfg: tlsconfig): bool

Releases the configuration and zeroes any key material in it.

## Connections

### tls.listen(port: i32; cfg: tlsconfig; cb: fn): bool

Binds a TCP socket on `port` and accepts TLS connections in a loop, invoking `cb` with each accepted `tlsconn`. Pass the callback by reference, `&handler`.

Returns `false` at once if the socket cannot be bound or the TLS context cannot be created. Otherwise it does not return: it runs until the process exits.

### tls.connect(host: str; port: i32; cfg: tlsconfig): ?(tlsconn)

Connects to `host:port` and performs a TLS 1.3 handshake. Returns the connection handle, or `0` if the connection, the handshake or the pin check fails.

### tls.read(conn: tlsconn): ?(str)

Reads available data. Returns `0` (the none sentinel) when the peer has closed the connection or an error occurred, so test with `==0` before use.

### tls.write(conn: tlsconn; data: str): bool

Writes `data`. `false` if the write failed, typically because the connection is closed.

### tls.close(conn: tlsconn): bool

Clean TLS shutdown followed by socket close. The handle must not be used afterwards.

### tls.protocol(conn: tlsconn): str

The protocol **negotiated on the wire**, read back through `SSL_get_version` -- `"TLSv1.3"` on any successful connection. This is a fact about the session, not a claim about the source, which is why `C022` asserts on it.

### tls.group(conn: tlsconn): str

The key-exchange group **negotiated on the wire**: `"X25519MLKEM768"` when the post-quantum hybrid was agreed, or the classical group the handshake fell back to (`"x25519"`, `"secp256r1"`, ...) when the peer did not offer the hybrid or the linked OpenSSL is older than 3.5. Empty for a handle that is not an open connection. The context prefers the hybrid but keeps classical groups for interoperability, and does not report a fallback on its own; a program that must not run over a classical key exchange compares it with `"X25519MLKEM768"` and closes the connection when it differs.

### tls.peercert(conn: tlsconn): ?(str)

The peer's PEM certificate as presented during the handshake, or `0` if the peer presented none.

### tls.fingerprint(pem: str): str

SHA-256 of the DER encoding of a PEM certificate, as 64 lowercase hex characters. Empty string if `pem` cannot be parsed. This is the same value as `openssl x509 -fingerprint -sha256`.

### tls.pairingcode(conn: tlsconn): str

A six-digit decimal code derived from the XOR of the local and peer certificate fingerprints. Both ends of a correctly established connection compute the same code, so it can be read aloud or displayed to confirm out of band that nobody is in the middle. Returns `"000000"` if either certificate is unavailable.

## A server

```toke
m=tlsserver;
i=tls:std.tls;
i=file:std.file;
i=io:std.io;
i=str:std.str;

f=rd(p:str):str{
  <mt file.read(p){$ok:v v;$err:e ""}
};

f=onconn(conn:i64):i64{
  io.println(str.concat("proto=";tls.protocol(conn)));

  let pc=tls.peercert(conn);
  if(pc==0){
    io.println("peercert=none")
  }el{
    io.println(str.concat("peerfp=";tls.fingerprint(pc)))
  };
  io.println(str.concat("pairing=";tls.pairingcode(conn)));

  let msg=tls.read(conn);
  if(msg==0){
    io.println("read=none")
  }el{
    tls.write(conn;str.concat("echo:";msg))
  };
  tls.close(conn);
  <0
};

(* mutualconfig, not three field assignments: the constructor is the
   documented route, and it makes "mutual auth" an intent rather than a
   boolean somebody forgets to set. *)
f=main():i64{
  let cfg=tls.mutualconfig(rd("/tmp/toke-tls-s.crt");rd("/tmp/toke-tls-s.key");rd("/tmp/toke-tls-c.crt"));
  io.println("listening on 8443");
  tls.listen(8443;cfg;&onconn);
  tls.freeconfig(cfg);
  <0
};
```

## A client

```toke
m=tlsclient;
i=tls:std.tls;
i=file:std.file;
i=io:std.io;
i=str:std.str;

f=rd(p:str):str{
  <mt file.read(p){$ok:v v;$err:e ""}
};

f=main():i64{
  (* present our own certificate AND pin the server's *)
  let cfg=tls.pinconfig(rd("/tmp/toke-tls-c.crt");rd("/tmp/toke-tls-c.key");rd("/tmp/toke-tls-s.crt"));

  let conn=tls.connect("127.0.0.1";8443;cfg);
  if(conn==0){
    io.println("connect=fail");
    tls.freeconfig(cfg);
    <1
  };

  io.println(str.concat("proto=";tls.protocol(conn)));
  io.println(str.concat("pairing=";tls.pairingcode(conn)));
  tls.write(conn;"ping");

  let r=tls.read(conn);
  if(r==0){
    io.println("reply=none")
  }el{
    io.println(str.concat("reply=";r))
  };

  tls.close(conn);
  tls.freeconfig(cfg);
  <0
};
```

## Security notes

- **TLS 1.3 only**, enforced as a version floor and ceiling. No downgrade is reachable.
- **A client with no pin does not authenticate the server.** `tls.tlsconfig` on the connecting side accepts any certificate (see `tls.tlsconfig` above). Use `tls.pinconfig` or `tls.mutualconfig` whenever the peer's identity matters.
- **Pinning** (`tls.pinconfig`) gives an identity guarantee stronger than a CA chain, and is the right choice for device-to-device or service-to-service links where you control both ends.
- **Mutual TLS** (`tls.mutualconfig`) authenticates both ends. Combined with pinning it is the strongest configuration here, and it fails closed: an unauthenticated peer never reaches your handler.
- **Pairing codes** are six digits. They are a human-verification convenience, not a substitute for comparing full fingerprints where the stakes justify it.
- Generated keys are **P-384** (NIST secp384r1), about 192-bit security, and the recommended curve for new deployments.
- `tls.keyof` returns private key material. Do not log it; prefer writing it straight to a file with restrictive permissions, or holding it in `std.securemem`.

## See Also

- `std.http` -- HTTPS, when you want a protocol on top rather than a raw encrypted stream.
- `std.securemem` -- holding the private key in locked, wiped memory.
- `std.crypto` -- hashing and signing primitives.
