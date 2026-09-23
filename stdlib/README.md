# toke standard library

The interface files (`*.tki`) in this directory are the **published surface** of the
toke standard library. Each one declares the exports of one module, and they are
generated from the compiler's own builtin table (story 137.12) rather than
maintained by hand.

## Where the documentation is

**[`docs/stdlib/`](../docs/stdlib/)** — one page per module, and the only place a
module is documented.

This README deliberately does **not** list the modules. It used to list six of
them, at a time when there were sixty-five, and it went on listing six for long
enough that a reader arriving here got a confident answer that was wrong by an
order of magnitude. A directory README that enumerates anything drifts; this
project has closed the same defect enough times to stop writing new instances of
it (135.11, 135.13, 136.52, 136.53).

For the current count and the module list, read `docs/stdlib/index.md`, or derive
it — `ls stdlib/*.tki`.

## Using a module

Import it by path, binding it to a local alias:

    i=http:std.http;
    i=db:std.db;

The lowercase `i=` is the v0.4 form. The uppercase `I=` this file used to show was
retired by the v0.4 break and no longer parses.

## Normative signatures

The signatures declared here are normative per the
[toke specification](../spec/). **Changes to a signature require a spec amendment
before the change is made here**, not after.

Two gates hold this directory to that:

- `make check-tki` — every declaration resolves to a defined C symbol, and
  declares the arity the compiler actually enforces (135.17).
- `make check-tki-names` — no declaration names an identifier the default
  59-character profile cannot express, since `_` is not in it (136.46).

## Licence

See [`LICENSING.md`](../LICENSING.md) in the repository root. Do not assume the
terms from this directory alone: the toke repository is **mixed-licence**, and the
root `LICENSE` file is not the whole answer.
