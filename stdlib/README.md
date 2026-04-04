# toke-stdlib

The standard library for [toke](https://github.com/karwalski/toke-spec).

## Modules

| Module | Description |
|--------|-------------|
| `std.str` | String operations |
| `std.http` | HTTP server and client |
| `std.db` | Database access |
| `std.json` | JSON encode and decode |
| `std.file` | File I/O |
| `std.net` | TCP socket operations |

## Using the stdlib

Import any module by its path:

    I=http:std.http;
    I=db:std.db;

## Normative signatures

The function signatures in this library are normative per the
[toke specification](https://github.com/karwalski/toke-spec).
Changes to signatures require a spec amendment before being made here.

## Licence

MIT. Compiled binaries that link against toke-stdlib carry no
licence obligations.
