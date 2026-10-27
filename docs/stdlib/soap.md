---
title: std.soap
slug: soap
section: reference/stdlib
order: 56
---

**Status: Implemented** -- C runtime backing (`src/stdlib/soap.c`).

The `std.soap` module builds SOAP 1.1 envelopes and faults, extracts the body
or fault from a response, and builds WS-Security header fragments. Every
function takes and returns `$str`. The normative signatures are in
[stdlib-signatures](/docs/spec/stdlib-signatures/#stdsoap) (131.48). Pair it
with [std.xml](/docs/stdlib/xml/) to build bodies and read values.

## Functions

### soap.envelope(body: $str; header: $str): $str

Wraps `body` in a SOAP 1.1 `soap:Envelope` with an XML declaration, adding a
`soap:Header` only when `header` is not empty. Both are inserted verbatim.

### soap.fault(code: $str; string: $str; detail: $str): $str

Returns a `soap:Fault` element with `faultcode`, `faultstring` and, when
`detail` is not empty, `detail`. All three are XML-escaped.

### soap.extractbody(envelope: $str): $str

Returns the inner XML of the first `soap:Body`, `SOAP-ENV:Body` or unprefixed
`Body` element, or `""` when there is none.

### soap.extractfault(envelope: $str): $str

Returns the whole first `soap:Fault`, `SOAP-ENV:Fault` or unprefixed `Fault`
element, or `""` when the envelope carries no fault.

### soap.wsseusernametoken(user: $str; pass: $str): $str

Returns a `wsse:Security` element holding a `UsernameToken` with a
PasswordText password. Both values are XML-escaped; nothing is hashed.

### soap.wssetimestamp(created: $str; expires: $str): $str

Returns a `wsu:Timestamp` element with the given `Created` and `Expires`
text, XML-escaped. It reads no clock.

## Example

```toke
m=example;
i=io:std.io;
i=soap:std.soap;
i=xml:std.xml;

f=main():i64{
  let env=soap.envelope(xml.element("GetPrice";xml.element("Item";"Widget";"");"");"");
  let body=soap.extractbody(env);
  io.println(xml.get(body;"GetPrice.Item"));
  if(soap.extractfault(env)==""){io.println("no fault")};
  <0
};
```
