---
title: std.math
slug: math
section: reference/stdlib
order: 25
---

**Status: Implemented** -- C runtime backing.

The `std.math` module provides numeric utility functions covering aggregation, descriptive statistics, simple linear regression, and standard math operations. All aggregate functions operate on arrays of `f64` values; scalar functions operate on individual `f64` values.

## Types

### $linregresult

Returned by `math.linreg`. Contains the least-squares fit line and a goodness-of-fit measure.

| Field | Type | Meaning |
|-------|------|---------|
| slope | f64 | Slope of the regression line |
| intercept | f64 | Y-intercept of the regression line |
| r2 | f64 | R-squared coefficient of determination (0.0 to 1.0) |

## Functions

### math.sum(xs: @(f64)): f64

Returns the sum of all values in the array. Returns `0.0` for an empty array.

```toke
m=test;
i=math:std.math;
f=example():void{
  let total = math.sum(@(1.0; 2.0; 3.0; 4.0));
  let empty = math.sum(@());
};
```

### math.mean(xs: @(f64)): f64

Returns the arithmetic mean of the values in the array. Returns `NaN` for an empty array.

```toke
m=test;
i=math:std.math;
f=example():void{
  let avg = math.mean(@(10.0; 20.0; 30.0));
};
```

### math.median(xs: @(f64)): f64

Returns the median value of the array without modifying the original order. For arrays with an even number of elements, returns the average of the two middle values. Returns `NaN` for an empty array.

```toke
m=test;
i=math:std.math;
f=example():void{
  let m = math.median(@(3.0; 1.0; 4.0; 1.0; 5.0));
  let e = math.median(@(2.0; 4.0));
};
```

### math.stddev(xs: @(f64)): f64

Returns the population standard deviation of the values in the array. Returns `NaN` for an empty array. Use `math.variance` if you need the variance first.

```toke
m=test;
i=math:std.math;
f=example():void{
  let sd = math.stddev(@(2.0; 4.0; 4.0; 4.0; 5.0; 5.0; 7.0; 9.0));
};
```

### math.variance(xs: @(f64)): f64

Returns the population variance of the values in the array. Returns `NaN` for an empty array. This is the square of `math.stddev`.

```toke
m=test;
i=math:std.math;
f=example():void{
  let v = math.variance(@(2.0; 4.0; 4.0; 4.0; 5.0; 5.0; 7.0; 9.0));
};
```

### math.percentile(xs: @(f64); p: f64): f64

Returns the p-th percentile of the values in the array, where `p` is in the range `[0, 100]`. Returns `NaN` for an empty array or if `p` is outside `[0, 100]`.

```toke
m=test;
i=math:std.math;
f=example():void{
  let p50 = math.percentile(@(1.0; 2.0; 3.0; 4.0; 5.0); 50.0);
  let p90 = math.percentile(@(1.0; 2.0; 3.0; 4.0; 5.0); 90.0);
};
```

### math.linreg(xs: @(f64); ys: @(f64)): $linregresult

Fits a least-squares linear regression line to the paired `xs` and `ys` data and returns a `$linregresult` containing the slope, intercept, and R-squared value. Both arrays must have the same length; if they differ, slope and intercept are `NaN` and `r2` is `0.0`.

```toke
m=test;
i=math:std.math;
f=example():void{
  let xs  = @(1.0; 2.0; 3.0; 4.0; 5.0);
  let ys  = @(2.1; 4.0; 5.9; 8.1; 9.8);
  let fit = math.linreg(xs; ys);
};
```

### math.min(xs: @(f64)): f64

Returns the smallest value in the array. Returns `NaN` for an empty array.

```toke
m=test;
i=math:std.math;
f=example():void{
  let lo = math.min(@(3.0; 1.0; 4.0; 1.0; 5.0));
};
```

### math.max(xs: @(f64)): f64

Returns the largest value in the array. Returns `NaN` for an empty array.

```toke
m=test;
i=math:std.math;
f=example():void{
  let hi = math.max(@(3.0; 1.0; 4.0; 1.0; 5.0));
};
```

### math.abs(x: f64): f64

Returns the absolute value of `x`.

```toke
m=test;
i=math:std.math;
f=example():void{
  let a = math.abs(-7.5);
  let b = math.abs(3.0);
};
```

### math.sqrt(x: f64): f64

Returns the non-negative square root of `x`. Returns `NaN` for negative inputs.

```toke
m=test;
i=math:std.math;
f=example():void{
  let s = math.sqrt(9.0);
  let n = math.sqrt(-1.0);
};
```

### math.floor(x: f64): i64

Returns the largest integer less than or equal to `x`, rounded toward negative infinity.

```toke
m=test;
i=math:std.math;
f=example():void{
  let f1 = math.floor(3.9);
  let f2 = math.floor(-1.2);
};
```

### math.ceil(x: f64): i64

Returns the smallest integer greater than or equal to `x`, rounded toward positive infinity.

```toke
m=test;
i=math:std.math;
f=example():void{
  let c1 = math.ceil(3.1);
  let c2 = math.ceil(-1.9);
};
```

### math.pow(base: f64; exp: f64): f64

Returns `base` raised to the power `exp`. Follows IEEE 754 conventions: `math.pow(0.0; 0.0)` returns `1.0`.

```toke
m=test;
i=math:std.math;
f=example():void{
  let p = math.pow(2.0; 10.0);
  let r = math.pow(9.0; 0.5);
};
```

## Usage Examples

The examples below show a descriptive statistics pipeline and a linear regression workflow on a sample dataset.

```toke
m=main;
i=math:std.math;

f=main():i64{
  let scores = @(72.0; 85.0; 91.0; 68.0; 79.0; 95.0; 83.0; 77.0);

  let n   = math.sum(scores);
  let avg = math.mean(scores);
  let med = math.median(scores);
  let sd  = math.stddev(scores);
  let lo  = math.min(scores);
  let hi  = math.max(scores);
  let p75 = math.percentile(scores; 75.0);

  let xs  = @(1.0; 2.0; 3.0; 4.0; 5.0; 6.0; 7.0; 8.0);
  let ys  = @(72.0; 85.0; 91.0; 68.0; 79.0; 95.0; 83.0; 77.0);
  let fit = math.linreg(xs; ys);

  let roundedsd = math.floor(sd);
  let magnitude = math.abs(-42.0);
  let squared   = math.pow(sd; 2.0);
  <0;
};
```

## Dependencies

None.
