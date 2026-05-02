# toke Project Governance

**Version:** 0.1 (Draft)
**Effective date:** TBD
**Authored by:** Matt Watt ([@karwalski](https://github.com/karwalski))

---

## 1. Governance Model

### 1.1 Current Phase: Benevolent Dictator for Life (BDFL)

The toke project is currently governed under a BDFL model. Matt Watt holds final decision-making authority over the language specification, compiler implementation, standard library, tooling protocols, and all related repositories under the [karwalski](https://github.com/karwalski) GitHub account.

### 1.2 Transition Path: Technical Steering Committee

Upon any of the following triggers, governance shall transition from BDFL to a Technical Steering Committee (TSC):

- The specification reaches version 1.0 and a second independent compiler implementation passes the conformance suite.
- A consortium of two or more organisations formally adopts the specification (see story 4.5.2).
- The BDFL voluntarily transfers governance.

The TSC shall have a minimum of three and a maximum of seven members.

---

## 2. Decision-Making Process

### 2.1 Architecture Decision Records (ADRs)

Design decisions affecting the compiler architecture, standard library structure, or tooling interfaces shall be documented as ADRs in `docs/architecture/ADR-NNNN.md`.

### 2.2 Gate Reviews

| Gate | Criteria | Authority |
|------|----------|-----------|
| Gate 1 | Token efficiency > 10%, Pass@1 > 60% | BDFL |
| Gate 2 | Default syntax implemented, 7B model outperforms baseline | BDFL |
| Gate 3+ | Defined per-milestone in `docs/progress.md` | BDFL or TSC |

### 2.3 Lazy Consensus

For day-to-day decisions not covered by ADRs, RFCs, or gate reviews, a proposal is accepted if no objection is raised within 72 hours.

---

## 3. Versioning Policy

### 3.1 Specification Versioning

Semantic versioning (`MAJOR.MINOR.PATCH`). Version 1.0 requires: conformance suite passing for two independent implementations, ecosystem proof complete, all deferred sections resolved.

### 3.2 Compiler Versioning

Independent from the specification. Version string includes spec target: `tkc 0.4.2 (spec 0.1)`.

### 3.3 Standard Library Versioning

Versioned with the specification. Module additions are minor-version changes; removals or signature changes are major-version.

---

## 4. Backwards Compatibility Rules

### 4.1 During 0.x (Current)

The specification is unstable. Any aspect may change. Error code numbers and the diagnostic JSON schema are treated as stable by convention.

### 4.2 After 1.0

| What | Can break in minor? | Can break in major? |
|------|---------------------|---------------------|
| Source language syntax | No | Yes, with migration guide |
| Error code meanings | No | No (codes may be deprecated but not reused) |
| Diagnostic JSON schema | No | Yes |
| Standard library signatures | No | Yes, with deprecation cycle |
| Compiler CLI flags | No | Yes |

### 4.3 Deprecation Cycle

Minimum two minor releases or 6 months between deprecation notice and removal.

---

## 5. Release Cadence

No fixed cadence during 0.x. Post-1.0: patches as needed, minor quarterly, major as accumulated.

---

## 6. Contribution Process

All contributions subject to Apache 2.0 licence and DCO. Rules from CONTRIBUTING.md apply. Non-negotiable: never change error code meanings, never populate `fix` with an incorrect fix, conformance suite must pass at 100%.

---

## 7. Security Disclosure

Report via security@tokelang.dev or GitHub private vulnerability reporting. Response within 48 hours, assessment within 7 days, public disclosure within 90 days.

---

## 8. Trademark and Naming

"toke" is always lowercase. Third parties may refer to the language but shall not imply official endorsement. Derivative compilers shall not be named "tkc".
