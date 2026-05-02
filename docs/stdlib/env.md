---
title: std.env
slug: env
section: reference/stdlib
order: 14
---

**Status: Implemented** -- C runtime backing.

The `std.env` module provides functions for reading and writing process environment variables. Keys and values are UTF-8 strings. Keys must not be empty or contain `=` or NUL characters. Changes made via `env.set` are visible to the current process only and are not inherited by previously-spawned children.

> **Note:** `env.tki` exports four functions: `env.get`, `env.get_or`, `env.set`, and the `$enverr` sum type. Functions documented in earlier versions (`env.unset`, `env.all`, `env.expand`, `env.args`) are not in the current tki and are not compiled — do not use them.

## Types

### $enverr

A sum type representing environment variable lookup failures.

| Variant | Meaning |
|---------|---------|
| $notfound | The environment variable is not set in the current process |
| $invalid | The key is empty or contains an invalid character (`=`, NUL) |

## Functions

### env.get(key: $str): $str!$enverr

Looks up the environment variable `key` and returns its value on success. Returns `$enverr.$notfound` if the variable is not set, or `$enverr.$invalid` if the key is empty or contains `=` or a NUL character.

```toke
m=example;
i=env:std.env;

f=showpath():i64{
  let r=env.get("PATH");
  r|{
    $ok:v  0;
    $err:e 1
  }
};
```

### env.get_or(key: $str; default: $str): $str

Looks up `key` and returns its value when the variable is set, or `default` when the variable is absent or the key is invalid. This function is always infallible.

```toke
m=example;
i=env:std.env;

f=getport():$str{
  let port=env.get_or("PORT";"8080");
  < port
};
```

### env.set(key: $str; val: $str): bool

Sets the environment variable `key` to `val` in the current process, overwriting any existing value. Returns `true` on success, or `false` if the key is empty, contains `=` or NUL, or if the underlying OS call fails.

```toke
m=example;
i=env:std.env;

f=setmode():bool{
  let ok=env.set("APP_MODE";"production");
  < ok
}
```

## Usage Examples

Read `PORT`, `HOST`, and `DEBUG` environment variables with defaults, then conditionally configure log level based on `DEBUG`:

```toke
m=config;
i=env:std.env;
i=log:std.log;

f=configure():i64{
  let port=env.get_or("PORT";"3000");
  let host=env.get_or("HOST";"0.0.0.0");
  let debug=env.get_or("DEBUG";"false");

  if(debug="true"){
    log.setlevel("debug")
  }el{
    log.setlevel("info")
  };

  let r=env.get("API_SECRET");
  let ok=r|{$ok:s 1;$err:e 0};
  if(ok=0){
    log.error("API_SECRET is required";@());
    <1
  }el{
    log.info("secret loaded";@())
  };
  <0
};
```

## See Also

- [std.str](/docs/stdlib/str) -- string manipulation for parsing variable values
