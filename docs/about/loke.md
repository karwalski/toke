---
title: loke
slug: loke
section: about
order: 7
---

A locally-run intelligence layer that sits between users, their data, and external LLMs -- minimising token spend, maximising privacy, and keeping sensitive data on the user's device.

- [GitHub](https://github.com/karwalski/loke)
- [Ecosystem overview](/ecosystem)

## What is loke?

loke is the intelligence layer of the toke ecosystem. It runs locally on your machine and intercepts the flow between your applications and external LLMs. Before any prompt leaves your device, loke compresses it -- stripping redundancy, caching repeated context, and routing cheap queries to smaller local models. The result: fewer tokens sent, lower cost, and sensitive data that never touches a remote server.

## Features

### Token optimisation

loke's compression pipeline strips whitespace, deduplicates repeated context, and summarises long histories. 60--80% reduction in tokens sent to the LLM API without sacrificing response quality.

### Privacy by default

Sensitive data -- PII, credentials, internal documents -- is detected and redacted before leaving the device. You decide what goes to the cloud and what stays local.

### Policy control

Define routing rules: send code completion to a fast local model, send complex reasoning to a frontier model, block certain data categories entirely. Policy lives in a config file you own.

### Companion devices

loke can coordinate across a local network of devices -- offloading compute to a nearby Mac Studio or NAS while keeping the interface on your laptop.

### MCP integration

loke integrates with the Model Context Protocol (MCP), acting as a local MCP server that any MCP-compatible client can connect to. toke's own MCP tools route through loke by default.

### Browser and terminal

loke works in two modes: terminal daemon for CLI and IDE integration, and browser extension for web-based LLM interfaces. Both share the same local policy engine.

## How it works

loke sits as a transparent proxy between your tools and the LLM API. Applications point to `localhost:loke` instead of the API endpoint directly. Outbound requests are compressed and filtered. Responses are cached and decompressed. The application sees a normal LLM API -- it doesn't need to know loke is there.

loke is part of the [toke ecosystem](/ecosystem). It is a separate project from toke the language.
