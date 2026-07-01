# Epic 120 — Security Audit

This directory holds the working framework and reports for the Epic 120 security
audit of the in-scope toke repositories (toke, toke-mcp, toke-website,
toke-cloud, and related toolchain surfaces).

Reconnaissance of entry points, transports, deployed artifacts, and trust
boundaries is in [`recon.md`](recon.md).

> PUBLIC-REPO RULE: everything in this directory lives in the **public** `toke`
> repo. Do not include IP addresses, hostnames, SSH usernames, credential/key
> file paths, or other operational infra detail. Refer to infrastructure
> indirectly ("the deploy host", "the managed cloud edge", "the API endpoint").

---

## Severity scheme

Severities are **CVSS v3.1-informed** but assigned by reviewer judgement (a full
vector is optional; record one when it clarifies the call). Use the exploitation
impact under the finding's reachability, not the theoretical worst case.

| Severity | Rough CVSS band | Meaning |
|---|---|---|
| **Critical** | 9.0–10.0 | Remote unauthenticated RCE, auth bypass, secret disclosure, or full compromise of a production surface. Fix immediately. |
| **High** | 7.0–8.9 | Serious impact but with a precondition (auth required, specific config, or partial impact such as sandbox escape with limits). |
| **Medium** | 4.0–6.9 | Meaningful weakness needing chained conditions or yielding limited impact (info leak, DoS with effort, missing hardening with a real path to harm). |
| **Low** | 0.1–3.9 | Minor issue, hard-to-exploit, or low impact (verbose errors, weak defaults behind other controls). |
| **Info** | 0.0 | No direct security impact; hardening recommendation, defence-in-depth, or hygiene. |

## Reachability tags

Every finding is tagged with **who can trigger it**. Reachability strongly
influences severity — the same bug is Critical if `remote-unauth` and Low if
`build-time`.

| Tag | Who can reach it |
|---|---|
| `remote-unauth` | Any network client with no credentials (e.g. an open SSE/HTTP endpoint). |
| `remote-auth` | A network client holding valid credentials / an API key (e.g. an authenticated managed-service caller). |
| `local` | An actor with local access to the host or process (operator, co-tenant, local editor/agent over stdio). |
| `build-time` | Reachable only during build, deploy, or CI (deploy scripts, server-side compile step, image build). |

A finding may carry more than one tag; rate it at its most-exposed reachability.

---

## Workflow: findings become stories

**Every confirmed finding becomes a story in Epic 121**, numbered `121.N`
(e.g. `121.1`, `121.2`, ...). The audit (Epic 120) produces findings; the
remediation work is tracked as Epic 121 stories. This keeps the audit
non-destructive: reviewers do not fix issues in place — they file them.

Rules:
- One story per distinct finding. Do not batch unrelated issues into one story.
- Reference the finding's report file and severity/reachability in the story.
- Info-level hardening items still become stories (they can be low-priority),
  so nothing is silently dropped.

(Progress-tracker updates for the 121.N stories are made by the main thread, not
by audit agents. Do not edit `progress.md` from within the audit.)

---

## Report layout

Reports live in **this** directory:

- `recon.md` — shared reconnaissance (this pass).
- `<area>.md` — **one report file per audited area / story**, e.g.
  `toke-mcp.md`, `toke-website.md`, `toke-cloud.md`, `tkc-compiler.md`,
  `deploy-pipeline.md`. Each collects the findings for that area.
- `index.md` — the **roll-up**: a single table of all findings across areas with
  severity, reachability, the mapped `121.N` story, and status. `index.md` is the
  canonical entry point once the audit is underway.

### Finding template (use inside each `<area>.md`)

```
### F-<area>-<n>: <short title>
- Severity: <Critical|High|Medium|Low|Info>
- Reachability: <remote-unauth|remote-auth|local|build-time>
- Story: 121.<N>
- Location: <repo>/<path> (no infra secrets)
- Summary: <one-sentence defect statement>
- Failure scenario: <concrete inputs/state → wrong/unsafe outcome>
- Recommendation: <fix direction>
```

Keep failure scenarios concrete and reproducible; prefer a verified
proof-of-reachability over a theoretical one.
