---
title: The *oke Ecosystem
slug: ecosystem
section: about
order: 5
---

A layered stack of tools, frameworks, and runtimes -- each named with the `*oke` convention. Four characters, always pronounceable, always part of the family.

## toke — language

The core programming language. Compiles to native code via LLVM. 59-character alphabet, 14 keywords, backtrack-free grammar with bounded lookahead. Token efficiency is measured, not assumed: under cl100k_base toke costs 1.34x the tokens of equivalent Python on the 60 Gate-1 tasks (N = 60) and no shipped toke tokenizer yet beats cl100k_base on v0.4 text (N = 2,000) -- see `docs/metrics-baseline.md`. Gate 2 PASS: 100% compilation Pass@1 on a fine-tuned 7B model (curated set, v0.3 model).

- [Documentation](/docs)
- [GitHub](https://github.com/karwalski/toke)
- [Live tokenizer](/tokenizer)

## ooke — web framework

toke's web framework and static site generator, written in toke. File-system routing (`pages/`), a flat-file content store with model-validated frontmatter, and a template engine (`.tkt`) with layouts, blocks, partials and Markdown. One native binary with `ooke new`, `build`, `serve` and `run`; routes under `pages/api/` are request handlers served by `ooke serve`, not rendered into the static build.

ooke builds [tokelang.dev](https://tokelang.dev): `ooke build` renders the `/docs` pages, and a toke binary on `std.http` serves the site. It is written in and built with v0.4 toke. It is packaged as the release candidate **3.0.0-rc.1**, not yet published, and it still pins toke by commit rather than by version, because toke 3.0.0 — the version for the v0.4 break — is not yet published either. The binary links toke's C standard library and the system OpenSSL, zlib and SQLite libraries.

What is and is not a stable interface is written down in the repository's `docs/public-api.md`: semver covers the command line, `ooke.toml`, the project layout and the template syntax, not the internal module interfaces.

- [Project page](/ooke)
- [GitHub](https://github.com/karwalski/ooke)

## loke — intelligence layer

A locally-run intelligence layer that sits between users, their data, and external LLMs. Multi-layer PII detection, token optimisation, intelligent routing, governance controls, memory palace, MCP framework. All processing on-device.

- [Project page](/loke)
- [GitHub](https://github.com/karwalski/loke)

## moke — demo application

Data analysis demo exercising loke's privacy pipeline, governance controls, and LLM integration end-to-end. 26-detector analysis engine, 9 Australian-themed datasets, client-side ML. Integrated into the loke project.

- [GitHub (in loke)](https://github.com/karwalski/loke)

## aoke — human experience (reserved)

The topmost layer. Sound, light, perception -- the interface between computation and the senses. UX, design systems, accessibility, and sensory analytics.

## zoke — hardware layer (reserved)

The bottommost layer. Silicon, electrons, circuits -- the physical substrate of computation. Drivers, ISA, FPGA, and everything below the OS.

## The *oke naming convention

Every tool in the toke ecosystem uses a single consonant prefix followed by `oke`. Four characters, always pronounceable. The alphabetical ordering maps roughly to abstraction depth -- `zoke` at the hardware layer, `aoke` at the human experience layer -- giving the stack a built-in mnemonic.

| Layer | Name | Status | Description |
|-------|------|--------|-------------|
| Human experience | aoke | Reserved | UX, design systems, accessibility |
| Intelligence | loke | **Production** | Privacy, token optimisation, routing |
| Web | ooke | **Production** | Static site gen, web framework (builds tokelang.dev; std.http serves it); 3.0.0-rc.1, unpublished |
| Language | toke | **Production** | Compiler, spec, stdlib (67 modules) |
| Data/demo | moke | **Active** | Analysis demo (in loke) |
| Hardware | zoke | Reserved | Drivers, ISA, FPGA |
