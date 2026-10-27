---
title: std.soap
slug: soap
section: reference/stdlib
order: 66
---

**Status: Implemented** -- C runtime backing (`src/stdlib/soap.c`, `src/stdlib/soap_glue.c`).

The `std.soap` module builds SOAP 1.1 envelopes and faults, extracts the body or fault from a received envelope, and builds the two WS-Security header blocks most services ask for. It produces and searches **strings**: there is no HTTP transport (send the envelope with [std.http](/docs/stdlib/http/)), no WSDL handling and no XML parser -- extraction is a substring search for the tag names listed below. Pair it with [std.xml](/docs/stdlib/xml/) to build the body.

The interface (`stdlib/soap.tki`) is generated from the C glue (`"gen":"abi"`): arity is checked, every parameter is declared `i64`. The types in the headings are the ones the implementation reads. A null argument is treated as `""`.

## Escaping

`soap.fault` and both WS-Security builders XML-escape every argument (`&`, `<`, `>`, `"`, `'`). `soap.envelope` does **not** escape: its arguments are XML fragments placed verbatim.

## Functions

### soap.envelope(body: $str; header: $str): $str

Returns a SOAP 1.1 envelope (namespace `http://schemas.xmlsoap.org/soap/envelope/`, prefix `soap`):

```
<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">
  <soap:Header>HEADER</soap:Header>
  <soap:Body>BODY</soap:Body>
</soap:Envelope>
```

The `<soap:Header>` line is omitted when `header` is empty. SOAP 1.2 envelopes exist in the C layer but are not exported.

### soap.fault(code: $str; message: $str; detail: $str): $str

Returns a SOAP 1.1 `<soap:Fault>` element with `<faultcode>`, `<faultstring>` and, when `detail` is non-empty, `<detail>`. All three values are escaped as text. Place the result in an envelope body with `soap.envelope`.

### soap.extractbody(envelope: $str): $str

Returns the text between the first `<soap:Body ...>` and the next `</soap:Body>`; failing that, the same for `SOAP-ENV:Body`, then for an unprefixed `Body`. Returns `""` when none is found. Other namespace prefixes are not recognised.

### soap.extractfault(envelope: $str): i64

Returns the first complete `<soap:Fault>...</soap:Fault>` element (or `SOAP-ENV:Fault`, or unprefixed `Fault`), including its tags, or `0` when the envelope carries no fault. The result is declared `i64` -- a string handle or `0` -- so test it against `0` before using it; interpolating it prints a number (131.48).

### soap.wsseusernametoken(username: $str; password: $str): $str

Returns a `<wsse:Security>` block containing a `<wsse:UsernameToken>` with the username and a **plain-text** password (`#PasswordText`). There is no digest, nonce or created timestamp. Pass the result as the `header` of `soap.envelope`.

### soap.wssetimestamp(created: $str; expires: $str): $str

Returns a `<wsu:Timestamp>` block with the two values as `<wsu:Created>` / `<wsu:Expires>`. The values are not validated; ISO 8601 UTC (`2024-01-01T00:00:00Z`) is what WS-Security expects.

## Example

```toke
m=example;
i=io:std.io;
i=soap:std.soap;

f=main():i64{
  let env=soap.envelope("<x>1</x>";"");
  io.println(soap.extractbody(env));
  let f=soap.fault("Server";"boom";"a<b");
  io.println(f);
  <0
};
```

Prints `<x>1</x>`, then a fault whose detail reads `a&lt;b`.
