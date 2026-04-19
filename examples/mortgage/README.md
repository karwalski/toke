# Mortgage Calculator

A CLI mortgage calculator that computes monthly payments and prints a full
amortisation schedule. Supports saving and loading scenarios as JSON files.

## Files

| File | Purpose |
|------|---------|
| `model.tk` | Data types: `$scenario`, `$payment`, `$result`, `$calcerr` |
| `calc.tk` | Core calculation: validation, monthly payment formula, amortisation loop |
| `main.tk` | CLI interface: prompts, formatting, JSON serialisation, entry point |

## Build

```bash
tkc main.tk calc.tk model.tk -o mortgage
```

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
Total interest:   637722.40
Total cost:       1137722.40

Month    Payment    Principal  Interest   Balance
-----  ----------  ----------  ---------  ----------
1      3160.34    1493.67    2708.33    498506.33
2      3160.34    1501.76    2700.24    497004.57
...
```

The calculator will offer to save the scenario to a JSON file for later
reuse.

## Features

- Standard amortisation formula: `M = P * [r(1+r)^n] / [(1+r)^n - 1]`
- Handles zero-interest edge case
- Optional extra monthly payments (accelerated payoff)
- Input validation with typed error variants
- JSON scenario save/load via `std.json` and `std.file`
