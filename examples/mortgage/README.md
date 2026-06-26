# Mortgage Calculator

A CLI mortgage calculator that computes the monthly payment and prints a full
amortisation schedule.

## Files

| File | Purpose |
|------|---------|
| `model.tk` | Data types: `$scenario`, `$payment`, `$result`, `$calcerr` |
| `calc.tk` | Core calculation: validation, monthly payment formula, amortisation loop |
| `main.tk` | CLI interface: prompts, formatting, entry point |

## Build

```bash
./build.sh        # writes ./mortgage
```

toke has no single-command multi-module link yet (the older `tkc *.tk -o`
form is unimplemented — see toke story 114.40), so `build.sh` emits each
module to LLVM IR and links them with the stdlib via `--emit-deps`.

## Usage

Run the calculator interactively:

```
$ ./mortgage
=== Toke Mortgage Calculator ===

Principal amount: 500000
Annual interest rate (e.g. 0.065 for 6.5%): 0.065
Loan term in years: 30
Extra monthly payment (0 for none): 0

Monthly payment:  3160.34
Total interest:   637722.44
Total cost:       1137722.44

Month    Payment    Principal  Interest   Balance
-----  ----------  ----------  ---------  ----------
1      3160.34    452.01    2708.33    499547.99
2      3160.34    454.46    2705.88    499093.54
...
360      3160.34    3143.31    17.03    0.00
```

Each month's interest is `balance * (annual_rate / 12)`; the principal portion
is `payment - interest`, and the balance amortises to `0.00` at month 360.

## Features

- Standard amortisation formula: `M = P * [r(1+r)^n] / [(1+r)^n - 1]`
- Handles zero-interest edge case
- Optional extra monthly payments (accelerated payoff)
- Input validation with typed error variants (`$calcerr`)
