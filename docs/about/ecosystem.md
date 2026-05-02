---
title: The *oke Ecosystem
slug: ecosystem
section: about
order: 5
---

A layered stack of tools, frameworks, and runtimes -- each named with the `*oke` convention. Four characters, always pronounceable, always part of the family.

## toke — language

The core programming language, purpose-built for LLM code generation. 55-character syntax, zero runtime, single binary. Compiles via LLVM to native code on any platform.

- [Documentation](/docs)
- [GitHub](https://github.com/karwalski/toke)

## ooke — web framework

A CMS and web framework built on toke. File-system routing, flat-file content store, template engine with Markdown, build and serve modes. Zero dependencies. Ships as a single binary.

- [Project page](/ooke)
- [GitHub](https://github.com/karwalski/ooke)

## loke — intelligence layer

A locally-run intelligence layer that sits between users, their data, and external LLMs. Minimises token spend, maximises privacy, and keeps sensitive data on the user's device. 60--80% token reduction.

- [Project page](/loke)
- [GitHub](https://github.com/karwalski/loke)

## aoke — human experience (reserved)

The topmost layer. Sound, light, perception -- the interface between computation and the senses. UX, design systems, accessibility, and sensory analytics.

## zoke — hardware layer (reserved)

The bottommost layer. Silicon, electrons, circuits -- the physical substrate of computation. Drivers, ISA, FPGA, and everything below the OS.

## moke — playground (internal)

The donkey. Demo environments, sandboxes, and experimental features. The thing you ride when you're just messing around. Originally a demo of loke.

## The *oke naming convention

Every tool in the toke ecosystem uses a single consonant prefix followed by `oke`. Four characters, always pronounceable. The alphabetical ordering maps roughly to abstraction depth -- `zoke` at the hardware layer, `aoke` at the human experience layer -- giving the stack a built-in mnemonic.
