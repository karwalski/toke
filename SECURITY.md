# Security Policy

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Report vulnerabilities by email or through GitHub's private reporting feature:

- **Email:** security@tokelang.dev
- **GitHub:** Use "Report a vulnerability" in the Security tab of this repository.

All reports are handled by Matthew Watt ([@karwalski](https://github.com/karwalski)).

## What to Include in a Report

- Description of the vulnerability and its potential impact
- Affected component (compiler, corpus pipeline, model) and version
- Steps to reproduce, including a minimal test case if possible
- Any suggested fix or workaround you have identified

## Response Timeline

| Stage | Target |
|-------|--------|
| Acknowledgement | 72 hours |
| Triage | 7 days |
| Fix | 90 days |
| Public disclosure | After fix, coordinated |

If a fix requires longer than 90 days, we will notify you and agree on an
extended timeline before the deadline passes.

## Scope

### In scope

- The tkc compiler (all released versions)
- The corpus generation pipeline
- Released toke language models

### Out of scope

- Third-party dependencies (report to their respective maintainers)
- User-written toke programs
- Issues in test or benchmark infrastructure that do not affect production

## Security Contact

Email: security@tokelang.dev
GitHub: Use "Report a vulnerability" in the Security tab of this repository.
CVE process: See [docs/security/cve-process.md](docs/security/cve-process.md).
