---
title: oke Namespace Specification
slug: oke-namespace
section: spec
order: 10
---

# *oke namespace specification

**Project:** toke ecosystem — [tokelang.dev](https://tokelang.dev) | [github.com/karwalski/toke](https://github.com/karwalski/toke)
**Status:** Draft v0.1
**Date:** 2026-04-06
**Audience:** Product and development teams

---

## Purpose

The toke ecosystem uses the `*oke` naming convention — a single consonant prefix followed by `oke` — to name layers, tools, and services across the stack. This document establishes the namespace registry, records linguistic and cultural due diligence on each letter, defines the layer architecture, and reserves slots for future expansion.

The convention is intentionally compact: four characters, always pronounceable, always recognisable as part of the toke family. The alphabetical ordering maps naturally to abstraction depth — `zoke` at the hardware layer, `aoke` at the human experience layer — giving the stack a built-in mnemonic.

---

## Etymology of "oke"

The suffix `oke` carries several benign meanings across languages, none of which conflict with the project's identity:

- **English (1920s slang):** Clipping of "OK" — affirmation, approval.
- **West African (Mande):** The phrase "o ke" means "it is so" — an affirmative discourse marker.
- **South African English:** "Oke" means a guy, a bloke — from Afrikaans "outjie" (little chap).
- **Greek/Turkish/Arabic:** An old unit of weight (~2.8 pounds), spelled "oke" or "oka."
- **Middle English:** Obsolete spelling of "oak."
- **Indonesian/Malay:** Borrowed from English "OK" — means the same thing.
- **Edo (Nigerian):** "Òké" means "hill."
- **Esperanto:** Exists as a word in Esperanto dictionaries.

No offensive, trademarked, or culturally sensitive meanings were found for `oke` itself.

---

## Assigned names

These names are actively in use or committed for development.

### toke — the language
The core programming language, purpose-built for LLM code generation. Fewer tokens, faster inference, lower cost.
**Traditional stack equivalent:** Programming language and compiler (Rust, Go, TypeScript).

### ooke — CMS and web framework
A CMS and web framework built on the toke language.
**Traditional stack equivalent:** Web framework (Next.js, Django, Rails, Express).

### loke — intelligence layer
A locally-run intelligence layer that sits between users, their data, and external LLMs — minimising token spend, maximising privacy, and keeping sensitive data on the user's device.
**Traditional stack equivalent:** AI/ML inference layer (LangChain, vector DB, RAG pipeline).

### aoke — human experience physics
The topmost layer of the stack. Concerned with sound waves, light waves, perception, and tactics for the human experience. The interface between computation and the senses.
**Traditional stack equivalent:** UX, design systems, accessibility, analytics.

### zoke — fundamental compute physics
The bottommost layer of the stack. Concerned with hardware, silicon, electrons, circuits — the physical substrate of computation.
**Traditional stack equivalent:** Hardware, silicon, drivers, ISA, FPGA.

### moke — fun layer / playground
The donkey. Demo environments, sandboxes, playgrounds, experimental features. Originally a demo of loke. Retained as the project workhorse — the thing you ride when you're just messing around.
**Traditional stack equivalent:** Sandbox, REPL, playground, demo environment.

### woke — principles charter
Reserved for the open source, transparent, free and fair principles of the toke projects. Not a software layer — a governance and values layer.
**Traditional stack equivalent:** Licensing, governance, OSS foundation charter (Apache, CNCF).

---

## Reserved names

These names are held for likely future use. Their proposed roles are suggestions, not commitments.

| Name | Proposed role | Rationale | Traditional equivalent |
|------|--------------|-----------|----------------------|
| doke | Data / state management | No significant conflicts found. Minor Urban Dictionary slang only. | State management (Redux, Zustand, signals) |
| hoke | Mock / test framework | English "hoke" means "to fake/exaggerate." Lean into it. | Testing framework (Jest, Vitest, mocks) |
| ioke | I/O and integrations | I/O + oke is a natural compound. Clean namespace. | API clients, SDKs, third-party integrations |
| joke | Easter eggs and fun | English word. Reserve for April Fools features and developer delight. | Developer experience / DX tooling |
| koke | Performance and benchmarking | A spin on "coke" energy without the trademark/slang baggage. | Profiling, APM, observability (Lighthouse, Datadog) |
| noke | Networking / node layer | Clean — no conflicts found in any language surveyed. | Networking, HTTP, service mesh (Envoy, nginx) |
| qoke | Queue / message layer | Q + oke maps naturally to queue semantics. Unusual spelling keeps it distinctive. | Message queue (RabbitMQ, Kafka, SQS) |
| roke | Caching / ephemeral storage | English "roke" means fog, mist, vapour. Evocative of data that appears and disappears. | Cache layer (Redis, Memcached, CDN edge) |
| soke | Auth / governance / permissions | English "soke" is an old legal term for a jurisdiction or district. Maps to access control. | Auth, IAM, permissions (OAuth, RBAC, JWT) |
| voke | Virtualisation / containers | Clean namespace. V naturally maps to VM/virtualisation. | Container runtime (Docker, Wasm, VM isolation) |
| xoke | Extensions / plugin system | X maps to "extension" across many tech contexts. Clean namespace. | Plugin / extension API (LSP, WASI, kernel modules) |
| yoke | Binding / orchestration | English "yoke" means a coupling device. Natural fit for connecting services. | Orchestration (Kubernetes, Terraform, systemd) |

---

## Open names

These names have no conflicts and no proposed role. They are available for future assignment.

| Name | Notes |
|------|-------|
| eoke | Clean. Awkward double-vowel opening. Could suit events/streaming. |
| foke | Clean. Sounds like "folk" in some accents. Could suit community features. |
| uoke | Clean. Vowel-heavy, slightly awkward to pronounce. Could suit utilities/stdlib. |

---

## Avoided names

These names are permanently excluded from the namespace due to trademark, cultural, or linguistic conflicts.

### boke — avoid
**Reason:** In Japanese, ボケ (boke) means "idiot" or "fool." It is widely known through anime, manga, and the manzai comedy tradition (the boke is the "stupid" half of a comedy duo). This meaning is globally recognised in tech-adjacent communities. Using this name for a product would invite ridicule and undermine credibility, particularly in Asian markets.

### coke — avoid
**Reason:** "Coke" is a registered trademark of The Coca-Cola Company and is also widely used as slang for cocaine. Both associations are strong enough to make this name unusable for any serious product.

### goke — avoid
**Reason:** Too phonetically close to "gook," a serious racial slur against people of East and Southeast Asian descent. The slur has roots in the Korean and Vietnam Wars and remains highly offensive. Even though "goke" is not itself a slur, the proximity is too close for comfort, particularly in international contexts.

### poke — avoid (soft)
**Reason:** "Poke" is a common English verb (to prod/jab), was a prominent Facebook feature, and overlaps with Pokémon branding. While not offensive, the namespace is heavily overloaded. Branding and SEO would be difficult. Could be reconsidered for internal tooling only if needed.

---

## Layer architecture

The `*oke` namespace maps to a layered architecture. The alphabetical order provides a rough guide to abstraction depth: letters closer to Z sit nearer the hardware, letters closer to A sit nearer the human.

```
┌─────────────────────────────────────────────────┐
│  aoke   Human experience (perception, UX, light, sound)    │
├─────────────────────────────────────────────────┤
│          Application & presentation layers                  │
│  doke?  eoke?  foke?                                        │
├─────────────────────────────────────────────────┤
│          Intelligence & logic layers                        │
│  hoke?  ioke?  joke   koke?  loke   moke                   │
├─────────────────────────────────────────────────┤
│          Middleware & services layers                        │
│  noke?  ooke   qoke?  roke?  soke?                          │
├─────────────────────────────────────────────────┤
│          Core language & runtime                            │
│  toke   uoke?  voke?                                        │
├─────────────────────────────────────────────────┤
│          System, governance & infrastructure                │
│  woke   xoke?  yoke?                                        │
├─────────────────────────────────────────────────┤
│  zoke   Fundamental compute physics (hardware, silicon)     │
└─────────────────────────────────────────────────┘

Legend: name = assigned | name? = reserved/proposed
        Avoided: boke, coke, goke, poke
```

The z-to-a ordering is a guideline, not a strict rule. Some names will inevitably serve cross-cutting concerns (e.g. woke as a principles charter applies to every layer). The primary purpose of the ordering is to make the stack intuitive and memorable.

---

## Naming rules

When assigning a new `*oke` name, follow these checks:

1. **Check this registry first.** If the name is listed as avoided, do not use it. If it is reserved, discuss with the team whether the proposed role still fits.
2. **Search for conflicts.** Before committing a new name, search for meanings in English, Japanese, Korean, Chinese, Spanish, Portuguese, Hindi, Arabic, Indonesian, and Afrikaans at minimum. Check Urban Dictionary, Wiktionary, and the OED.
3. **Check trademarks.** Search USPTO, EUIPO, and IP Australia for the four-letter name in software/technology classes.
4. **Prefer names with resonant existing meanings.** A name like "yoke" (coupling) or "roke" (mist) that aligns with its technical role is stronger than a completely empty name.
5. **Avoid names that are common English words with strong existing associations** (joke is the exception because it leans into the association deliberately).
6. **Update this document** when any name is assigned, reserved, or released.

---

## Revision history

| Date | Version | Change |
|------|---------|--------|
| 2026-04-06 | 0.1 | Initial draft. Established namespace registry, linguistic audit, layer architecture, and naming rules. |
