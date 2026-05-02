# toke language spec — condensed reference (v0.3)

File ext: .tk | Compiler: tkc | All lowercase, no uppercase anywhere.

## character set (55)

26 lowercase: a-z
10 digits: 0-9
19 symbols: ( ) { } = : . ; + - * / < > ! | $ @ %
String delimiter `"` consumed by lexer, not structural.
`&` appears as function reference prefix (`&name`), consumed by lexer.
Whitespace is insignificant (token separator only).
No underscores. No `^` or `~` (bitwise deferred to v0.5+).

## keywords (13)

Context keywords (top-level + `=`): m f t i
Reserved keywords: if el lp br let mut as rt mt

## no comments

toke source is comment-free by design. There is no comment syntax.
Documentation for human readers lives in companion files (`.tkc`), separate from source.
Never generate comments in toke output.

## declarations (file order: m, i, t, f)

```
m=mymod.sub;                           # module — required, first
i=str:std.str;                         # import — alias:path
t=$point{x:i64;y:i64};                # struct type
t=$err{$notfound:u64;$bad:$str};      # sum type — $-prefixed variants
f=add(a:i64;b:i64):i64{<a+b};         # function
f=load(p:$str):$str!$fileerr{...};    # fallible function
```

Note: `#` annotations above are spec explanations only. toke has no comment syntax.

## types

Scalars (no sigil): i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool byte
Strings: $str
User-defined: $user $point (always $lowercase)
Array type: @i64 @$str @$user
Map type: @(i64:$str) @($str:$str)
Result: :$rettype!$errtype (sugar for $result{$ok:$t;$err:$e})

## operators

Arithmetic: + - * / %
Comparison: < > = (single = for equality in expressions)
Logical: && || ! (short-circuit; operands must be bool)
Bitwise: & | ^ ~ << >>
Precedence (high to low): ~ ! - (unary) > * / % > + - > << >> > & > ^ > | > < > = > && > ||
Type cast: expr as i64
No implicit coercion. No == or !=. Use !(a=b) for not-equal.

## control flow

```
# if/else
if(x>0){<x}el{<0};

# loop — the only loop construct
lp(let i=mut.0;i<n;i=i+1){
  if(i=5){br};    # br exits innermost loop
};

# return — two forms
<expr;             # short return — preferred
rt expr;           # long return — for nested contexts
```

## bindings

```
let x=42;                 # immutable
let name=mut."default";   # mutable — mut. prefix on value
name="updated";           # reassign mutable
let x=99;                 # shadowing allowed in same scope
```

## error handling

```
# fallible function signature
f=getuser(id:u64):$user!$usererr{
  r=db.query(dbhandle;"SELECT ...") !$usererr;   # propagate
  <$user{id:r.id;name:r.name}
};

# match on result
mt getuser(1) {
  $ok:u   log.info(u.name);
  $err:e  log.error("fail")
};

# sum type match — must be exhaustive
mt val {
  $notfound:id  <"missing";
  $bad:msg      <msg
};
```

## function references

```
# &name passes function as value
http.get("/users";&listusers);
http.post("/users";&createuser);
```

## strings

Double-quoted. Escapes: \" \\ \n \t \r \0 \xNN.
Interpolation: "\(expr)" where expr resolves to $str.
Semicolons separate args, never commas: f(a;b;c).

## struct and collection literals

```
let p=$point{x:1;y:2};           # struct
let a=@(1;2;3);                  # array
let m=@("a":1;"b":2);            # map
```

Access: p.x, a.get(i), a.len, m.get("a").
No square brackets. No a[i].

## identifiers

Pattern: [a-z][a-z0-9]*
No underscores. No uppercase anywhere.

## style rules

- All lowercase, $prefix for types, @prefix for arrays/maps.
- ; terminates every statement and separates list items.
- No commas anywhere.
- No whitespace significance.
- One canonical form per construct — no synonyms.
- Each file: one module decl, imports, types, functions (in that order).

## stdlib modules

std.str: len concat slice split upper lower trim contains replace starts ends
std.json: enc dec str i64 f64 bool arr parse
std.toon: enc dec str i64 f64 bool arr fromjson tojson
std.yaml: enc dec str i64 f64 bool arr fromjson tojson
std.http: param header res.ok res.json res.bad res.err http.get http.post http.put http.delete http.client
std.db: open exec query close
std.file: read write append list delete
std.log: info warn error debug
std.env: get set
std.process: exec spawn wait kill
std.crypto: sha256 sha512 hmacsha256 hmacsha512 constanteq randombytes tohex
std.time: now fmt since
std.test: eq neq ok fail
std.ws: ws.connect ws.send ws.recv ws.close ws.broadcast
std.sse: sse.emit sse.emitdata sse.close sse.keepalive
std.i18n: load get fmt locale

## common mistakes to avoid

| Wrong | Correct | Why |
|---|---|---|
| == | = | Single = for both assignment and comparison |
| != | !(a=b) | No not-equal operator |
| [1,2,3] | @(1;2;3) | No square brackets; semicolons not commas |
| a[i] | a.get(i) | No bracket indexing |
| User, Str | $user, $str | No uppercase; $ sigil required |
| fn foo() | f=foo() | Declaration keyword is f= |
| return x | <x or rt x | No return keyword |
| else if | el{if(c){...}} | No elif; nest if inside el{} |
| mut x=0 | let x=mut.0 | Mutable uses mut. prefix on value |
| f(a,b) | f(a;b) | Semicolons separate arguments |
| Ok(v) | $ok{v} | Variants are $lowercase |

## complete example

```
m=app.main;
i=str:std.str;
i=log:std.log;
i=file:std.file;

t=$config{path:$str;verbose:bool};

f=readconfig(path:$str):$config!$fileerr{
  data=file.read(path)!$fileerr;
  let lines=str.split(data;"\n");
  let verbose=str.contains(data;"verbose=true");
  <$config{path:path;verbose:verbose}
};

f=main():i64{
  mt readconfig("app.conf") {
    $ok:c   log.info(str.concat("loaded: ";c.path));
    $err:e  log.error("config failed")
  };
  <0
};
```
