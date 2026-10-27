---
title: Standard Library
slug: index
section: reference/stdlib
order: 0
---

API reference for every module in the toke standard library. Each page documents the public functions, types, and error codes for one `std.*` module.

## Core

- [std.str](/docs/stdlib/str/) -- string manipulation
- [std.fmt](/docs/stdlib/fmt/) -- value formatting for output
- [std.io](/docs/stdlib/io/) -- console I/O: stdout, stderr, stdin lines
- [std.math](/docs/stdlib/math/) -- mathematical functions
- [std.time](/docs/stdlib/time/) -- date, time, and duration
- [std.file](/docs/stdlib/file/) -- file system operations
- [std.path](/docs/stdlib/path/) -- file path manipulation
- [std.env](/docs/stdlib/env/) -- environment variables
- [std.process](/docs/stdlib/process/) -- process management
- [std.os](/docs/stdlib/os/) -- thin POSIX bridge: descriptors, errno
- [std.args](/docs/stdlib/args/) -- command-line argument parsing
- [std.log](/docs/stdlib/log/) -- structured logging
- [std.test](/docs/stdlib/test/) -- test framework
- [std](/docs/stdlib/std/) -- the root module: `std.homedir`
- [std.sys](/docs/stdlib/sys/) -- platform, CPU, RAM, disk, config/data directories
- [std.task](/docs/stdlib/task/) -- run functions on a thread pool, grouped in scopes
- [std.mem](/docs/stdlib/mem/) -- raw, unchecked memory: alloc, copy, byte load/store

## Collections

- [std.array](/docs/stdlib/array/) -- module-call spelling of the array built-ins
- [std.vec](/docs/stdlib/vec/) -- mutable growable vector, in-place set
- [std.stack](/docs/stdlib/stack/) -- LIFO container of `i64`
- [std.queue](/docs/stdlib/queue/) -- FIFO container of `i64`
- [std.set](/docs/stdlib/set/) -- unique `i64` values
- [std.collections](/docs/stdlib/collections/) -- three array helpers (duplicates of the array built-ins)

## Data Formats

- [std.json](/docs/stdlib/json/) -- JSON parsing and serialization
- [std.json_stream](/docs/stdlib/json_stream/) -- streaming JSON
- [std.csv](/docs/stdlib/csv/) -- CSV parsing
- [std.yaml](/docs/stdlib/yaml/) -- YAML support
- [std.toml](/docs/stdlib/toml/) -- TOML support
- [std.toon](/docs/stdlib/toon/) -- TOON (toke object notation)
- [std.encoding](/docs/stdlib/encoding/) -- base64 and hex encoding
- [std.md](/docs/stdlib/md/) -- Markdown processing
- [std.zip](/docs/stdlib/zip/) -- read-only zip archive access
- [std.xlsx](/docs/stdlib/xlsx/) -- read cell values out of an XLSX workbook
- [std.pdf](/docs/stdlib/pdf/) -- extract text runs with positions from a PDF
- [std.xml](/docs/stdlib/xml/) -- build XML text and read values by tag path

## Networking

- [std.http](/docs/stdlib/http/) -- HTTP client and server
- [std.net](/docs/stdlib/net/) -- low-level networking
- [std.ws](/docs/stdlib/ws/) -- WebSocket support
- [std.sse](/docs/stdlib/sse/) -- Server-Sent Events
- [std.router](/docs/stdlib/router/) -- HTTP routing
- [std.soap](/docs/stdlib/soap/) -- SOAP 1.1 envelopes, faults, WS-Security headers

## Security

- [std.crypto](/docs/stdlib/crypto/) -- cryptographic primitives
- [std.crypto_ext](/docs/stdlib/crypto_ext/) -- extended cryptography
- [std.encrypt](/docs/stdlib/encrypt/) -- encryption utilities
- [std.auth](/docs/stdlib/auth/) -- authentication
- [std.tls](/docs/stdlib/tls/) -- standalone TLS 1.3 connections
- [std.keychain](/docs/stdlib/keychain/) -- OS credential store
- [std.securemem](/docs/stdlib/securemem/) -- mlock'd, wiped, TTL-expiring secret buffers

## UI and Visualisation

- [std.html](/docs/stdlib/html/) -- HTML generation
- [std.template](/docs/stdlib/template/) -- template engine
- [std.svg](/docs/stdlib/svg/) -- SVG generation
- [std.canvas](/docs/stdlib/canvas/) -- canvas drawing
- [std.chart](/docs/stdlib/chart/) -- chart generation
- [std.dashboard](/docs/stdlib/dashboard/) -- dashboard components
- [std.image](/docs/stdlib/image/) -- image processing
- [std.ocr](/docs/stdlib/ocr/) -- platform text recognition (macOS today; `$noengine` elsewhere)

## Data and AI

- [std.db](/docs/stdlib/db/) -- database access
- [std.dataframe](/docs/stdlib/dataframe/) -- tabular data
- [std.analytics](/docs/stdlib/analytics/) -- analytics utilities
- [std.llm](/docs/stdlib/llm/) -- LLM integration
- [std.llmtool](/docs/stdlib/llmtool/) -- LLM tool definitions
- [std.ml](/docs/stdlib/ml/) -- machine learning
- [std.vecstore](/docs/stdlib/vecstore/) -- embedded vector store with cosine search

## Internationalisation

- [std.i18n](/docs/stdlib/i18n/) -- internationalisation support

## Not usable today

These modules are published but are not reachable from a toke program. Each page opens with a status banner saying exactly what links, what does not, and what restoring it costs. They are listed here so the gap is visible from the index rather than discovered at the linker.

- [std.infer](/docs/stdlib/infer/) -- local llama.cpp inference. Façade: complete C core, all-stub glue.
- [std.mlx](/docs/stdlib/mlx/) -- Apple Silicon MLX bridge. Façade: two functions have no wrapper and fail at link.
- [std.mdns](/docs/stdlib/mdns/) -- Bonjour/DNS-SD discovery. Façade: two functions have no wrapper; `browse` has lost its callback parameter.
- [std.webview](/docs/stdlib/webview/) -- native browser window. Withdrawn (136.2); cannot be imported at all.

One more module links and runs but does nothing:

- [std.clipboard](/docs/stdlib/clipboard/) -- stub. `read` always returns `""`; `write` discards its text.
