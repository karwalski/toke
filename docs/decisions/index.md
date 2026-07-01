---
title: Architecture Decision Records
slug: index
section: decisions
order: 0
---

Architectural decisions for the toke project, recorded as ADRs.

## Decisions

- [ADR-0001 — Initial Language and Compiler Architecture](/docs/decisions/ADR-0001/) -- foundational architecture choices
- [ADR-0002 — Source File Extension](/docs/decisions/ADR-0002-file-extension/) -- `.tk` file extension decision
- [ADR-0003 — Serialization Format Strategy and Internationalisation](/docs/decisions/ADR-0003/) -- TOON, YAML/JSON, and i18n
- [ADR-0004 — String building uses interpolation + stdlib, never operator overloading](/docs/decisions/ADR-0004/) -- `+` is numeric-only
- [ADR-0005 — ooke is pure toke; native capability lives in toke core](/docs/decisions/ADR-0005/) -- the app contains zero C; gaps become reusable stdlib
- [ADR-0006 — Array mutation performance: capacity + copy-on-write](/docs/decisions/ADR-0006/) -- **proposed** (story 114.18); fixes O(N²) array building
- [ADR-0007 — `if` is an expression](/docs/decisions/ADR-0007/) -- v0.4 (116/A1); expression-`if` replaces the mut-flag idiom
- [ADR-0008 — `=` is assignment, `==` is equality](/docs/decisions/ADR-0008/) -- v0.4 (116/A3); breaking; removes the `=` overload
- [ADR-0009 — Backtrack-free parser (bounded lookahead) + `&&`/`||`](/docs/decisions/ADR-0009/) -- v0.4 (116/A2,A4,A5); honest grammar property
- [Gate 1 Decision Document](/docs/decisions/gate1-decision/) -- Gate 1 review and outcome
