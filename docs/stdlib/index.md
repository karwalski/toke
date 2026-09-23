---
title: Standard Library
slug: index
section: reference/stdlib
order: 0
---

API reference for every module in the toke standard library. Each page documents the public functions, types, and error codes for one `std.*` module.

## Core

- [std.str](/docs/reference/stdlib/str/) -- string manipulation
- [std.math](/docs/reference/stdlib/math/) -- mathematical functions
- [std.time](/docs/reference/stdlib/time/) -- date, time, and duration
- [std.file](/docs/reference/stdlib/file/) -- file system operations
- [std.path](/docs/reference/stdlib/path/) -- file path manipulation
- [std.env](/docs/reference/stdlib/env/) -- environment variables
- [std.process](/docs/reference/stdlib/process/) -- process management
- [std.args](/docs/reference/stdlib/args/) -- command-line argument parsing
- [std.log](/docs/reference/stdlib/log/) -- structured logging
- [std.test](/docs/reference/stdlib/test/) -- test framework

## Data Formats

- [std.json](/docs/reference/stdlib/json/) -- JSON parsing and serialization
- [std.json_stream](/docs/reference/stdlib/json_stream/) -- streaming JSON
- [std.csv](/docs/reference/stdlib/csv/) -- CSV parsing
- [std.yaml](/docs/reference/stdlib/yaml/) -- YAML support
- [std.toml](/docs/reference/stdlib/toml/) -- TOML support
- [std.toon](/docs/reference/stdlib/toon/) -- TOON (toke object notation)
- [std.encoding](/docs/reference/stdlib/encoding/) -- base64 and hex encoding
- [std.md](/docs/reference/stdlib/md/) -- Markdown processing
- [std.zip](/docs/reference/stdlib/zip/) -- read-only zip archive access

## Networking

- [std.http](/docs/reference/stdlib/http/) -- HTTP client and server
- [std.net](/docs/reference/stdlib/net/) -- low-level networking
- [std.ws](/docs/reference/stdlib/ws/) -- WebSocket support
- [std.sse](/docs/reference/stdlib/sse/) -- Server-Sent Events
- [std.router](/docs/reference/stdlib/router/) -- HTTP routing

## Security

- [std.crypto](/docs/reference/stdlib/crypto/) -- cryptographic primitives
- [std.crypto_ext](/docs/reference/stdlib/crypto_ext/) -- extended cryptography
- [std.encrypt](/docs/reference/stdlib/encrypt/) -- encryption utilities
- [std.auth](/docs/reference/stdlib/auth/) -- authentication
- [std.tls](/docs/reference/stdlib/tls/) -- standalone TLS 1.3 connections
- [std.keychain](/docs/reference/stdlib/keychain/) -- OS credential store
- [std.securemem](/docs/reference/stdlib/securemem/) -- mlock'd, wiped, TTL-expiring secret buffers

## UI and Visualisation

- [std.html](/docs/reference/stdlib/html/) -- HTML generation
- [std.template](/docs/reference/stdlib/template/) -- template engine
- [std.svg](/docs/reference/stdlib/svg/) -- SVG generation
- [std.canvas](/docs/reference/stdlib/canvas/) -- canvas drawing
- [std.chart](/docs/reference/stdlib/chart/) -- chart generation
- [std.dashboard](/docs/reference/stdlib/dashboard/) -- dashboard components
- [std.image](/docs/reference/stdlib/image/) -- image processing

## Data and AI

- [std.db](/docs/reference/stdlib/db/) -- database access
- [std.dataframe](/docs/reference/stdlib/dataframe/) -- tabular data
- [std.analytics](/docs/reference/stdlib/analytics/) -- analytics utilities
- [std.llm](/docs/reference/stdlib/llm/) -- LLM integration
- [std.llmtool](/docs/reference/stdlib/llmtool/) -- LLM tool definitions
- [std.ml](/docs/reference/stdlib/ml/) -- machine learning
- [std.vecstore](/docs/reference/stdlib/vecstore/) -- embedded vector store with cosine search

## Internationalisation

- [std.i18n](/docs/reference/stdlib/i18n/) -- internationalisation support

## Not usable today

These modules are published but are not reachable from a toke program. Each page opens with a status banner saying exactly what links, what does not, and what restoring it costs. They are listed here so the gap is visible from the index rather than discovered at the linker.

- [std.infer](/docs/reference/stdlib/infer/) -- local llama.cpp inference. Façade: complete C core, all-stub glue.
- [std.mlx](/docs/reference/stdlib/mlx/) -- Apple Silicon MLX bridge. Façade: two functions have no wrapper and fail at link.
- [std.mdns](/docs/reference/stdlib/mdns/) -- Bonjour/DNS-SD discovery. Façade: two functions have no wrapper; `browse` has lost its callback parameter.
- [std.webview](/docs/reference/stdlib/webview/) -- native browser window. Withdrawn (136.2); cannot be imported at all.
