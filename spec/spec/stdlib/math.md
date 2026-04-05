# std.math

Scalar arithmetic, descriptive statistics, and simple linear regression.

## Types

```
type linregresult { slope: f64, intercept: f64, r2: f64 }
```

## Functions

```
f=sum(vals:[f64]):f64
f=mean(vals:[f64]):f64
f=median(vals:[f64]):f64
f=stddev(vals:[f64]):f64
f=variance(vals:[f64]):f64
f=percentile(vals:[f64];p:f64):f64
f=linreg(x:[f64];y:[f64]):linregresult
f=min(vals:[f64]):f64
f=max(vals:[f64]):f64
f=abs(v:f64):f64
f=sqrt(v:f64):f64
f=floor(v:f64):i64
f=ceil(v:f64):i64
f=pow(base:f64;exp:f64):f64
```

## Semantics

- `sum`, `mean`, `median`, `stddev`, `variance` operate on float arrays. Empty arrays return 0.0 for `sum` and NaN for the others.
- `stddev` computes the population standard deviation; `variance` computes population variance.
- `percentile` takes `p` in range [0.0, 100.0]. Values outside this range are clamped.
- `linreg` fits a least-squares line to paired x/y arrays. Arrays must be equal length; `r2` is the coefficient of determination.
- `min` / `max` on an empty array return +Inf / -Inf respectively.
- `sqrt` of a negative value returns NaN.
- `floor` / `ceil` truncate to i64.

## Dependencies

None.
