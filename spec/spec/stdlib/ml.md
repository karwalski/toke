# std.ml

Machine learning primitives: linear regression, k-means clustering, decision trees, and k-nearest neighbours.

## Types

```
type row         { vals: [f64] }
type linearmodel { coef: [f64], intercept: f64 }
type centroid    { id: u64, center: [f64] }
type kmeansmodel { centroids: [centroid], k: u64 }
type dtreemodel  { nodes: u64, maxdepth: u64, arena: u64 }  -- opaque
type mlerr       { msg: str }
```

## Functions

```
f=linregfit(features:[row];targets:[f64]):linearmodel!mlerr
f=linregpredict(model:linearmodel;features:[f64]):f64
f=kmeanstrain(data:[row];k:u64;maxiter:u64):kmeansmodel!mlerr
f=kmeansassign(model:kmeansmodel;point:[f64]):u64
f=dtreefit(data:[row];labels:[str];maxdepth:u64):dtreemodel!mlerr
f=dtreepredict(model:dtreemodel;features:[f64]):str
f=knnpredict(data:[row];labels:[str];query:[f64];k:u64):str
```

## Semantics

- `linregfit` fits a multivariate linear regression. `features` rows and `targets` must be equal length. Returns `mlerr` if the system is underdetermined.
- `linregpredict` computes the dot product of `model.coef` with `features` plus `model.intercept`.
- `kmeanstrain` runs Lloyd's algorithm for at most `maxiter` iterations with `k` clusters. Returns `mlerr` if `k` is zero or exceeds the number of data points.
- `kmeansassign` returns the index of the nearest centroid for the given point.
- `dtreefit` trains a classification decision tree with the given maximum depth. `labels` has one entry per row. Returns `mlerr` if `data` and `labels` lengths differ.
- `dtreepredict` returns the predicted class label for the given feature vector.
- `knnpredict` classifies `query` by majority vote of the `k` nearest rows (Euclidean distance). Does not require a training step.

## Dependencies

- `std.dataframe` (row-oriented data)
- `std.math` (distance and aggregation)
