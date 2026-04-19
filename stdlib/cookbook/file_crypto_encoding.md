# Cookbook: Secure File Handling

## Overview

Read a file from disk, compute a cryptographic hash of its contents, and base64 encode the result for safe transport over text protocols. Combines `std.file`, `std.crypto`, and `std.encoding` to demonstrate the common pattern of preparing file data for APIs, checksums, or digital signatures.

## Modules Used

- `std.file` -- File system read and write
- `std.crypto` -- Hashing and cryptographic operations
- `std.encoding` -- Base64 and hex encoding/decoding

## Complete Program

```toke
I std.file;
I std.crypto;
I std.encoding;
I std.str;
I std.json;

(* Read the input file as raw bytes *)
let path = "data/document.pdf";
let bytes = file.read_bytes(path)!;
let size = file.size(path)!;

(* Compute SHA-256 hash of the file contents *)
let hash = crypto.sha256(bytes);

(* Convert the hash to a hex string for display *)
let hash_hex = encoding.to_hex(hash);

(* Base64 encode the entire file for transport *)
let b64_content = encoding.to_base64(bytes);

(* Also compute an HMAC for integrity verification *)
let hmac_key = "shared-secret-key";
let hmac = crypto.hmac_sha256(hmac_key; bytes);
let hmac_hex = encoding.to_hex(hmac);

(* Build a JSON manifest describing the file *)
let manifest = json.new_object();
json.set(manifest; "filename"; json.from_str(path));
json.set(manifest; "size_bytes"; json.from_int(size));
json.set(manifest; "sha256"; json.from_str(hash_hex));
json.set(manifest; "hmac_sha256"; json.from_str(hmac_hex));
json.set(manifest; "content_base64"; json.from_str(b64_content));

(* Write the manifest to disk *)
let manifest_json = json.encode_pretty(manifest);
file.write_text("output/document_manifest.json"; manifest_json)!;

(* Demonstrate decoding: round-trip the base64 back to bytes *)
let decoded = encoding.from_base64(b64_content)!;
let verify_hash = crypto.sha256(decoded);
let verify_hex = encoding.to_hex(verify_hash);

if verify_hex != hash_hex {
    file.write_text("output/verify.log"; "FAIL: hash mismatch after round-trip")!;
} else {
    file.write_text("output/verify.log"; "PASS: round-trip hash matches")!;
};
```

## Step-by-Step Explanation

1. **Read file** -- `file.read_bytes` loads the entire file into a byte array. `file.size` returns the length in bytes for the manifest.

2. **Hash** -- `crypto.sha256` computes a SHA-256 digest of the raw bytes, returning a fixed-length byte array. This serves as a unique fingerprint of the file.

3. **Hex encoding** -- `encoding.to_hex` converts the binary hash into a human-readable hexadecimal string, the conventional format for displaying hashes.

4. **Base64 encoding** -- `encoding.to_base64` encodes the full file contents into a text-safe representation. This is essential when embedding binary data in JSON or transmitting over protocols that only support text.

5. **HMAC** -- `crypto.hmac_sha256` computes a keyed hash. Unlike a plain SHA-256, an HMAC proves that the sender knows the shared secret, providing both integrity and authentication.

6. **Manifest** -- The hash, HMAC, and encoded content are assembled into a JSON document. `json.encode_pretty` formats it with indentation for readability.

7. **Round-trip verification** -- The base64 string is decoded back to bytes and re-hashed. Comparing the two hashes confirms that encoding introduced no corruption.
