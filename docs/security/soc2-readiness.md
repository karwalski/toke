# SOC 2 Readiness Assessment — Draft

**Status:** Draft — conditional on hosted service launch
**Date:** 2026-04-03
**Story:** 4.6.2

---

## Scope

This assessment evaluates SOC 2 Type II readiness for a hypothetical hosted toke service — an API endpoint that accepts natural language descriptions, generates toke code via a fine-tuned model, compiles it with tkc, and returns a native binary or execution result.

SOC 2 Trust Service Criteria (TSC) assessed: Security, Availability, Processing Integrity, Confidentiality.

Privacy is excluded (toke does not process personal data in the compilation pipeline).

---

## Current State Summary

| Area | Status | Notes |
|------|--------|-------|
| Code signing | Implemented | sigstore cosign keyless; SBOM via syft (Story 3.7.1, 3.7.2) |
| Reproducible builds | Implemented | `make repro-check` verified; all object files bit-identical (Story 3.7.3) |
| Sandbox execution | Implemented | sandbox-exec (macOS) + Docker (Linux); no network, read-only FS, 10s timeout |
| Threat model | Documented | 5 threats (T1-T5) with mitigations (docs/security/threat-model.md) |
| Security audit scope | Documented | 4 scope areas; codebase map; accepted-risk register (Story 4.6.1) |
| Model safety eval | Implemented | LlamaGuard adversarial evaluation; 50 templates across 5 categories (Story 3.7.4) |
| CVE process | Documented | GitHub Security Advisories; CVSS thresholds defined (docs/security/cve-process.md) |
| Structured diagnostics | Implemented | 70+ error codes; JSON schema; no user data in errors |
| DCO enforcement | Implemented | All commits signed off; CONTRIBUTING.md |

---

## Gap Analysis by Trust Service Criteria

### CC1 — Control Environment

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC1.1 Board/management oversight | No formal governance structure | Establish advisory board or governance doc when hosted service is scoped |
| CC1.2 Risk assessment process | Threat model exists but no formal risk register | Promote threat-model.md to formal risk register with periodic review cadence |
| CC1.3 Policies and procedures | Security docs exist but no overarching security policy | Create security-policy.md consolidating all controls |

### CC2 — Communication and Information

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC2.1 Internal communication | Single developer; no gap | N/A until team grows |
| CC2.2 External communication | SECURITY.md, CVE process exist | Add incident response contact to hosted service docs |

### CC3 — Risk Assessment

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC3.1 Risk identification | Threat model covers corpus pipeline | Extend threat model to cover hosted API surface (auth, rate limiting, DDoS) |
| CC3.2 Fraud risk | Low risk — no financial transactions | Document as accepted risk |
| CC3.3 Change management | No formal change management process | Implement when hosted service launches |

### CC5 — Control Activities

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC5.1 Logical access | No hosted infrastructure yet | Implement IAM, least-privilege, MFA when infra is provisioned |
| CC5.2 Software development | DCO, CI, conformance tests in place | Add SAST to CI pipeline (cppcheck already available) |
| CC5.3 Infrastructure management | No production infrastructure | Define infra-as-code, monitoring, alerting when service launches |

### CC6 — Logical and Physical Access

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC6.1 Access provisioning | SSH keys, deploy keys per repo | Add key rotation schedule; document access grants |
| CC6.2 Access removal | Single developer; no process needed yet | Implement offboarding procedure when team grows |
| CC6.3 System boundary | Sandbox execution isolates compiled code | Document network isolation for hosted service |

### CC7 — System Operations

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC7.1 Vulnerability management | Secret scanning, dependency scanning in CI | Add runtime vulnerability scanning for hosted service |
| CC7.2 Incident detection | No monitoring infrastructure | Implement logging, alerting, SIEM when service launches |
| CC7.3 Incident response | CVE process defined; no operational incident response | Create incident response plan for hosted service SLA |

### CC8 — Change Management

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC8.1 Change authorisation | PR reviews not enforced (single developer) | Enforce branch protection rules; require reviews when team grows |

### CC9 — Risk Mitigation

| Control | Gap | Remediation |
|---------|-----|-------------|
| CC9.1 Vendor management | No third-party dependencies beyond LLVM and libc | Document vendor risk for cloud providers when hosting |

---

## Availability (A1)

| Control | Status | Notes |
|---------|--------|-------|
| A1.1 Capacity planning | Not applicable | No hosted service yet |
| A1.2 Recovery | Compiler is stateless; no data to recover | Hosted service will need backup/DR plan |
| A1.3 Testing | Conformance suite (90/90); e2e tests (9/9) | Add availability testing for hosted service |

---

## Processing Integrity (PI1)

| Control | Status | Notes |
|---------|--------|-------|
| PI1.1 Input validation | LL(1) parser rejects invalid input deterministically | Hosted service needs rate limiting and input size limits |
| PI1.2 Processing accuracy | Conformance tests, differential testing, Gate 1 benchmark | Compiler is the root of trust |
| PI1.3 Output completeness | Structured JSON diagnostics; deterministic output | No data loss risk in compilation |

---

## Confidentiality (C1)

| Control | Status | Notes |
|---------|--------|-------|
| C1.1 Data classification | No user data in compiler pipeline | Hosted service: user source code is confidential; define retention policy |
| C1.2 Data disposal | Stateless compiler; arena allocation freed on exit | Hosted service: define data retention and deletion procedures |

---

## Readiness Score

| Category | Ready | Gaps | Priority |
|----------|-------|------|----------|
| Security (CC) | Partial | 8 gaps — mostly deferred to hosted service launch | Medium |
| Availability (A) | Not started | All deferred to hosted service | Low (no service) |
| Processing Integrity (PI) | Strong | Rate limiting needed for hosted service | Low |
| Confidentiality (C) | Not started | Deferred to hosted service | Low (no service) |

**Overall:** The project has strong security foundations (sandbox, signing, threat model, CVE process, reproducible builds). Most SOC 2 gaps are infrastructure controls that only apply when a hosted service launches. No remediation is blocking for the current development-only phase.

---

## Recommendations

1. **Now (Phase 2):** Consolidate security docs into a single security-policy.md. Add key rotation schedule.
2. **Pre-hosted-service:** Extend threat model for API surface. Implement IAM, monitoring, incident response plan.
3. **Post-launch:** Engage SOC 2 Type II auditor. Implement continuous monitoring. Establish review cadence for risk register.
