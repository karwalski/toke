---
title: "Tutorial: Mortgage Calculator (CLI)"
slug: mortgage-cli
section: tutorials
order: 1
---

Build a fully working command-line mortgage calculator in toke. By the end you will have a program that collects loan details from stdin, computes compound interest and a full amortisation schedule, prints a formatted table, and saves/loads scenarios as JSON files.

**Time:** ~30 minutes
**Difficulty:** Beginner
**Prerequisites:** toke compiler installed, basic familiarity with toke syntax

---

## 1. What We're Building

The finished program does four things:

- **Interactive input** — prompts for principal, annual interest rate, loan term (years), and optional extra monthly payment.
- **Compound interest calculation** — standard formula `M = P * [r(1+r)^n] / [(1+r)^n - 1]` with a zero-rate fallback.
- **Amortisation schedule** — month-by-month table showing payment, principal, interest, and remaining balance.
- **JSON save/load** — serialise a scenario to a JSON file so you can reload it later.

The project is split across three source files:

| File | Responsibility |
|------|---------------|
| `model.tk` | Type definitions: scenario, payment, result, error |
| `calc.tk` | Validation, monthly payment formula, amortisation loop |
| `main.tk` | CLI I/O, formatting, JSON serialisation, entry point |

---

## 2. LLM Prompts

You can recreate this project by feeding the following prompts to an LLM that knows toke syntax. Each prompt targets one file.

### Prompt 1 — Data Model

> Generate a toke module `mortgage.model` in a file called `model.tk`.
> Define four types:
>
> - `$scenario` — struct with `principal:f64`, `rate:f64`, `term:u64`, `extra:f64`
> - `$payment` — struct with `month:u64`, `payment:f64`, `principal:f64`, `interest:f64`, `balance:f64`
> - `$result` — struct with `monthly:f64`, `totalinterest:f64`, `totalcost:f64`, `schedule:@$payment`
> - `$calcerr` — sum type with variants `$invalidrate:f64`, `$invalidterm:u64`, `$fileerr:$str`
>
> Add a block comment above each type explaining its purpose.

### Prompt 2 — Calculation Engine

> Generate a toke module `mortgage.calc` in a file called `calc.tk`.
> Import `mortgage.model` as `m` and `std.math` as `math`.
> Implement three functions:
>
> - `validate(s:m.$scenario):bool!m.$calcerr` — return `$err` if rate is outside 0..1 or term is less than 1
> - `monthly(s:m.$scenario):f64` — standard compound interest formula; degenerate to `P/n` when rate is near zero
> - `amortise(s:m.$scenario):@(m.$payment)` — build the full schedule array, applying extra payments and capping at zero balance
> - `calculate(s:m.$scenario):m.$result!m.$calcerr` — validate, then return a `$result` with monthly, totals, and schedule
>
> Use `mut.` for mutable bindings and `lp` for loops.

### Prompt 3 — CLI Front End

> Generate a toke module `mortgage.main` in a file called `main.tk`.
> Import `std.io`, `std.json`, `std.file`, `std.str`, and the two mortgage modules.
> Implement:
>
> - `promptfloat(msg:str):f64` and `promptint(msg:str):u64` — read one value from stdin
> - `printsummary(r:m.$result):void` — display monthly payment, total interest, total cost
> - `printschedule(sched:@(m.$payment)):void` — formatted table with header
> - `scenariotojson(s:m.$scenario):str` and `jsontoscenario(raw:str):m.$scenario!m.$calcerr` — round-trip JSON
> - `savescenario` / `loadscenario` — file I/O wrappers
> - `run():void` — interactive main loop: prompt, calculate, print, optionally save
> - `main():i64` — entry point returning 0
>
> Handle every fallible call with `mt expr { $ok:...; $err:... }` match syntax.

### Prompt 4 (Optional) — Review Pass

> Review the three mortgage modules for correctness:
> - Does `amortise` cap the final payment when balance drops below the regular amount?
> - Does `jsontoscenario` return a clear error for every missing field?
> - Are all mutable bindings declared with `mut.`?

---

## 3. Generated Code

Below are the three source files exactly as they appear in `examples/mortgage/`.

### model.tk

```toke
m=mortgage.model;

t=$scenario{principal:f64;rate:f64;term:u64;extra:f64};

t=$payment{month:u64;payment:f64;principal:f64;interest:f64;balance:f64};

t=$result{monthly:f64;totalinterest:f64;totalcost:f64;schedule:@$payment};

t=$calcerr{$invalidrate:f64;$invalidterm:u64;$fileerr:$str};
```

**Key points:**

- `m=mortgage.model;` declares the module name. Every `.tk` file starts with one.
- `t=$scenario{...}` defines a struct type. Fields are separated by semicolons.
- `t=$calcerr{$invalidrate:f64;...}` defines a sum type (tagged union). Variant names start with `$`.
- `@$payment` is an array of `$payment` values.

### calc.tk

```toke
m=mortgage.calc;
i=m:mortgage.model;
i=math:std.math;

f=validate(s:m.$scenario):bool!m.$calcerr{
  if(s.rate<0.0||s.rate>1.0){
    <$err(m.$calcerr.$invalidrate(s.rate))
  };
  if(s.term<1){
    <$err(m.$calcerr.$invalidterm(s.term))
  };
  <$ok(true)
};

f=monthly(s:m.$scenario):f64{
  let n=s.term*12;
  let r=s.rate/12.0;
  if(r<0.000000001){
    <s.principal/(n as f64)
  };
  let rn=math.pow(1.0+r;(n as f64));
  <s.principal*(r*rn)/(rn-1.0)
};

f=amortise(s:m.$scenario):@(m.$payment){
  let pmt=monthly(s);
  let n=s.term*12;
  let bal=mut.s.principal;
  let sched=mut.@();
  lp(let i=1;i<(n as i64)+1;i=i+1){
    let mi=bal*(s.rate/12.0);
    let mp=pmt-mi+s.extra;
    if(mp>bal){
      mp=bal+mi
    };
    let princ=mp-mi;
    bal=bal-princ;
    if(bal<0.0){bal=0.0};
    let row=m.$payment{
      month:(i as u64);
      payment:mp;
      principal:princ;
      interest:mi;
      balance:bal
    };
    sched=sched.push(row);
    if(bal<0.01){
      <sched
    }
  };
  <sched
};

f=calculate(s:m.$scenario):m.$result!m.$calcerr{
  mt validate(s) {
    $ok:_ ();
    $err:e <$err(e)
  };
  let pmt=monthly(s);
  let sched=amortise(s);
  let n=sched.len;
  let totalcost=mut.0.0;
  let totalinterest=mut.0.0;
  lp(let i=0;i<(n as i64);i=i+1){
    let row=sched.get(i);
    totalcost=totalcost+row.payment;
    totalinterest=totalinterest+row.interest
  };
  <$ok(m.$result{
    monthly:pmt;
    totalinterest:totalinterest;
    totalcost:totalcost;
    schedule:sched
  })
};
```

**Key points:**

- `i=m:mortgage.model;` imports the model module under the alias `m`.
- `:bool!m.$calcerr` means the function returns `bool` on success or `m.$calcerr` on error (result type).
- `<$err(...)` and `<$ok(...)` are early returns wrapping the result.
- `let bal=mut.s.principal;` creates a mutable binding initialised from an immutable value.
- `lp(init;cond;step){...}` is toke's loop construct.
- `math.pow(base;exp)` calls the standard library power function. Arguments are separated by semicolons.
- `as f64` and `as i64` are type casts between numeric types.

### main.tk

```toke
m=mortgage.main;
i=io:std.io;
i=json:std.json;
i=file:std.file;
i=str:std.str;
i=calc:mortgage.calc;
i=m:mortgage.model;

f=formatcurrency(v:f64):str{
  let s=str.fromfloat(v);
  <s
};

f=printheader():void{
  io.println("Month    Payment    Principal  Interest   Balance");
  io.println("-----  ----------  ----------  ---------  ----------")
};

f=printrow(p:m.$payment):void{
  let line=str.buf();
  str.add(line;str.fromint(p.month as i64));
  str.add(line;"      ");
  str.add(line;formatcurrency(p.payment));
  str.add(line;"    ");
  str.add(line;formatcurrency(p.principal));
  str.add(line;"    ");
  str.add(line;formatcurrency(p.interest));
  str.add(line;"    ");
  str.add(line;formatcurrency(p.balance));
  io.println(str.done(line))
};

f=printsummary(r:m.$result):void{
  io.println("");
  io.println(str.concat("Monthly payment:  ";formatcurrency(r.monthly)));
  io.println(str.concat("Total interest:   ";formatcurrency(r.totalinterest)));
  io.println(str.concat("Total cost:       ";formatcurrency(r.totalcost)));
  io.println("")
};

f=printschedule(sched:@(m.$payment)):void{
  printheader();
  lp(let i=0;i<(sched.len as i64);i=i+1){
    let row=sched.get(i);
    printrow(row)
  }
};

f=scenariotojson(s:m.$scenario):str{
  let b=str.buf();
  str.add(b;"{");
  str.add(b;"\"principal\":");
  str.add(b;str.fromfloat(s.principal));
  str.add(b;",\"rate\":");
  str.add(b;str.fromfloat(s.rate));
  str.add(b;",\"term\":");
  str.add(b;str.fromint(s.term as i64));
  str.add(b;",\"extra\":");
  str.add(b;str.fromfloat(s.extra));
  str.add(b;"}");
  <str.done(b)
};

f=jsontoscenario(raw:str):m.$scenario!m.$calcerr{
  let doc=mt json.dec(raw) {
    $ok:d d;
    $err:e <$err(m.$calcerr.$fileerr("invalid json"))
  };
  let principal=mt json.f64(doc;"principal") {
    $ok:v v;
    $err:e <$err(m.$calcerr.$fileerr("missing principal"))
  };
  let rate=mt json.f64(doc;"rate") {
    $ok:v v;
    $err:e <$err(m.$calcerr.$fileerr("missing rate"))
  };
  let term=mt json.u64(doc;"term") {
    $ok:v v;
    $err:e <$err(m.$calcerr.$fileerr("missing term"))
  };
  let extra=mt json.f64(doc;"extra") {
    $ok:v v;
    $err:e <$err(m.$calcerr.$fileerr("missing extra"))
  };
  <$ok(m.$scenario{
    principal:principal;
    rate:rate;
    term:(term as u64);
    extra:extra
  })
};

f=savescenario(s:m.$scenario;path:str):bool!m.$calcerr{
  let data=scenariotojson(s);
  mt file.write(path;data) {
    $ok:v <$ok(true);
    $err:e <$err(m.$calcerr.$fileerr("could not write file"))
  }
};

f=loadscenario(path:str):m.$scenario!m.$calcerr{
  let raw=mt file.read(path) {
    $ok:v v;
    $err:e <$err(m.$calcerr.$fileerr("could not read file"))
  };
  <jsontoscenario(raw)
};

f=promptfloat(msg:str):f64{
  io.print(msg);
  let line=io.readln();
  let v=mt str.tofloat(str.trim(line)) {
    $ok:v v;
    $err:e 0.0
  };
  <v
};

f=promptint(msg:str):u64{
  io.print(msg);
  let line=io.readln();
  let v=mt str.toint(str.trim(line)) {
    $ok:v v;
    $err:e 0
  };
  <(v as u64)
};

f=run():void{
  io.println("=== Toke Mortgage Calculator ===");
  io.println("");

  let principal=promptfloat("Principal amount: ");
  let rate=promptfloat("Annual interest rate (e.g. 0.065 for 6.5%): ");
  let term=promptint("Loan term in years: ");
  let extra=promptfloat("Extra monthly payment (0 for none): ");

  let s=m.$scenario{
    principal:principal;
    rate:rate;
    term:term;
    extra:extra
  };

  mt calc.calculate(s) {
    $ok:r {
      printsummary(r);
      printschedule(r.schedule);

      io.println("");
      io.print("Save scenario to file? (y/n): ");
      let ans=str.trim(io.readln());
      if(str.startswith(ans;"y")){
        io.print("File path: ");
        let path=str.trim(io.readln());
        mt savescenario(s;path) {
          $ok:_ io.println("Scenario saved.");
          $err:e io.println("Error saving scenario.")
        }
      }
    };
    $err:e {
      io.println("Calculation error:");
      mt e {
        $invalidrate:v io.println(str.concat("  Invalid rate: ";str.fromfloat(v)));
        $invalidterm:v io.println(str.concat("  Invalid term: ";str.fromint(v as i64)));
        $fileerr:v     io.println(str.concat("  File error: ";v))
      }
    }
  }
};

f=main():i64{
  run();
  <0
};
```

**Key points:**

- `str.buf()` creates a mutable string buffer. Append with `str.add(buf;val)` and finalise with `str.done(buf)`.
- `io.print` writes without a newline; `io.println` adds one.
- `io.readln()` reads one line from stdin.
- `json.dec(raw)` parses a JSON string into a document, returning a result type.
- `json.f64(doc;"key")` extracts a float field from a JSON document by key.
- Every fallible call is handled with `mt expr { $ok:...; $err:... }` match syntax -- toke does not have exceptions.
- `f=main():i64` is the program entry point. Return `0` for success.

---

## 4. Build and Run

### Compile

```sh
tkc mortgage/model.tk mortgage/calc.tk mortgage/main.tk -o mortgage
```

The compiler processes all three files together, resolving cross-module references. The order of source files on the command line does not matter.

### Run

```sh
./mortgage
```

**Expected session:**

```
=== Toke Mortgage Calculator ===

Principal amount: 500000
Annual interest rate (e.g. 0.065 for 6.5%): 0.065
Loan term in years: 30
Extra monthly payment (0 for none): 0

Monthly payment:  3160.34
Total interest:   637722.40
Total cost:       1137722.40

Month    Payment    Principal  Interest   Balance
-----  ----------  ----------  ---------  ----------
1      3160.34    1493.67    1666.67    498506.33
2      3160.34    1498.65    1661.69    497007.68
3      3160.34    1503.64    1656.69    495504.04
...
360    3160.34    3149.84    10.50      0.00

Save scenario to file? (y/n): y
File path: my-mortgage.json
Scenario saved.
```

### Load a Saved Scenario

Saved files are plain JSON:

```json
{"principal":500000.0,"rate":0.065,"term":30,"extra":0.0}
```

You can edit them in any text editor and load them by extending the program (see exercises below).

---

## 5. Troubleshooting

### E2030: unresolved module

```
error[E2030]: unresolved module `mortgage.model`
```

The module name in `i=m:mortgage.model;` must match the `m=mortgage.model;` declaration at the top of `model.tk` exactly. Check for typos and ensure all three files are passed to `tkc`.

### E4031: type mismatch (f64 vs i64)

```
error[E4031]: expected `f64`, found `i64`
  --> calc.tk:20
```

Numeric types do not implicitly convert. Use `as f64` or `as i64` at the point of use:

```toke
let n=s.term*12;
s.principal/n

s.principal/(n as f64)
```

### E1006: uppercase keyword

```
error[E1006]: expected lowercase keyword, found `M`
```

All toke keywords are lowercase. Module declarations use `m=`, not `M=`. Function declarations use `f=`, not `F=`. Type declarations use `t=`, not `T=`.

### Missing semicolons

```
error: unexpected token
  --> main.tk:25
```

Statements inside blocks are separated by semicolons. The last statement in a block does not need one, but every statement before it does:

```toke
str.add(line;"hello")
str.add(line;" world")

str.add(line;"hello");
str.add(line;" world")
```

### Forgetting to handle result types

```
error[E4031]: expected `f64`, found `f64!$calcerr`
```

Any function returning `type!error` produces a result that must be matched before you can use the inner value:

```toke
let v=str.tofloat(line);

let v=mt str.tofloat(line) {
  $ok:v v;
  $err:e 0.0
};
```

### Mutable binding errors

```
error: cannot assign to immutable binding `bal`
```

Use `mut.` to create mutable bindings:

```toke
let bal=s.principal;
bal=bal-princ;

let bal=mut.s.principal;
bal=bal-princ;
```

---

## 6. Exercises

Once the base calculator is working, try extending it:

### Exercise 1: Biweekly Payments

Modify `calc.tk` to support biweekly payment mode (26 payments per year instead of 12). Add a `biweekly:bool` field to `$scenario` and adjust `monthly()` and `amortise()` to divide the year into 26 periods when enabled. Biweekly payments reduce total interest because you effectively make 13 monthly payments per year.

### Exercise 2: Comparison Mode

Add a `compare` function that takes two `$scenario` values and prints them side by side:

```
                   Scenario A    Scenario B
Principal:         500000.00     500000.00
Rate:              6.50%         5.75%
Term:              30 years      25 years
Monthly:           3160.34       3150.52
Total interest:    637722.40     445156.00
Total cost:        1137722.40    945156.00
Savings:                         192566.40
```

This requires building a second formatting path in `main.tk`.

### Exercise 3: CSV Export

Add a `save_csv` function that writes the amortisation schedule as a CSV file:

```csv
month,payment,principal,interest,balance
1,3160.34,1493.67,1666.67,498506.33
2,3160.34,1498.65,1661.69,497007.68
```