# Open Source Library Inventory

**Date:** 2026-04-19
**Story:** 57.5.1

## Vendored C Libraries (compiled into toke binaries)

| Library | Version | Licence | Source | Used by | Purpose |
|---------|---------|---------|--------|---------|---------|
| cmark | 0.31.1 | BSD-2-Clause | github.com/commonmark/cmark | ooke, toke-website | CommonMark → HTML rendering for markdown content pages |
| tomlc99 | 1.0 (obsoleted by tomlc17) | MIT | github.com/cktan/tomlc99 | ooke, hooks.c | TOML configuration file parsing |

## System Libraries (dynamically linked)

| Library | Version | Licence | Used by | Purpose |
|---------|---------|---------|---------|---------|
| OpenSSL | 3.6.2 | Apache-2.0 | ooke (TLS), http.c | HTTPS server, TLS client, crypto operations, ACME |
| SQLite3 | 3.51.0 | Public Domain | std.db | Embedded database backend |
| zlib | 1.2+ | zlib licence | tkc, ooke | HTTP response gzip compression |
| libm | System | System | tkc, stdlib | Math functions (sqrt, sin, cos, etc.) |

## npm Dependencies (toke-mcp)

| Package | Version | Licence | Purpose |
|---------|---------|---------|---------|
| @modelcontextprotocol/sdk | ^1.12.1 | MIT | MCP server framework |
| express | ^5.1.0 | MIT | HTTP API for console backend |

## Python Dependencies (toke-model, toke-eval)

| Package | Version | Licence | Purpose |
|---------|---------|---------|---------|
| sentencepiece | 0.2.0 | Apache-2.0 | BPE tokenizer training |
| anthropic | latest | MIT | LLM API for corpus refinement |
| transformers | latest | Apache-2.0 | Model training and inference |

## toke Licence

toke is dual-licensed under MIT and Apache-2.0. All vendored and linked libraries are compatible:
- cmark: BSD-2-Clause (permissive, compatible)
- tomlc99: MIT (compatible)
- OpenSSL 3.x: Apache-2.0 (compatible)
- SQLite: Public Domain (compatible)
- zlib: zlib licence (permissive, compatible)
- All npm packages: MIT (compatible)
- sentencepiece: Apache-2.0 (compatible)

**No copyleft or restricted licences found.**

## Notes

- tomlc99 is marked obsolete upstream. Successor is tomlc17 (same author, same licence). Migration recommended when convenient.
- OpenSSL version should be monitored for CVEs (high-value target).
- cmark version is from the commonmark/cmark repo; latest release should be checked periodically.
