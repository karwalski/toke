---
title: std.ml
slug: ml
section: reference/stdlib
order: 27
---

**Status: Implemented** -- C runtime backing.

The `std.ml` module provides classical machine learning algorithms: linear regression, k-means clustering, decision tree classification, k-nearest neighbors prediction, train/test splitting, and evaluation metrics including accuracy, precision, recall, and F1. All model training functions return result types so errors surface explicitly rather than silently.

## Types

### $row

Represents a single sample as an array of feature values.

| Field | Type | Meaning |
|-------|------|---------|
| vals | @(f64) | Feature values for one sample |

### $linearmodel

A fitted simple linear model produced by `ml.linregfit`.

| Field | Type | Meaning |
|-------|------|---------|
| coef | @(f64) | Coefficients for each feature |
| intercept | f64 | Bias / intercept term |

### $centroid

A single cluster center produced during k-means training.

| Field | Type | Meaning |
|-------|------|---------|
| id | u64 | Cluster identifier (0-based) |
| center | @(f64) | Centroid coordinates in feature space |

### $kmeansmodel

A trained k-means model holding all cluster centroids.

| Field | Type | Meaning |
|-------|------|---------|
| centroids | @($centroid) | One centroid per cluster |
| k | u64 | Number of clusters |

### $dtreemodel

Opaque handle to a trained decision tree. The internal structure (node array, split thresholds, leaf values) is managed by the runtime; use `ml.dtreepredict` to query it.

### $mlerr

Returned when a training function cannot complete, for example when the input data is empty or dimensions are inconsistent.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the failure |

## Functions

### ml.linregfit(X: @($row); y: @(f64)): $linearmodel!$mlerr

Fits a multivariate linear regression model to the training samples `X` and target values `y` using least squares. Returns `$mlerr` if `X` and `y` have different lengths or if the input is empty.

```toke
m=example;
i=ml:std.ml;

f=example():void{
  let ra = $row{vals:@(1.0; 2.0)};
  let rb = $row{vals:@(2.0; 3.0)};
  let rc = $row{vals:@(3.0; 5.0)};
  let feats = @(ra; rb; rc);
  let y = @(5.0; 8.0; 13.0);
  let model = ml.linregfit(feats; y)|{$ok:m m;$err:e $linearmodel{coef:@();intercept:0.0}};
};
```

### ml.linregpredict(model: $linearmodel; features: @(f64)): f64

Evaluates a fitted linear model at a single query point by computing the dot product of `features` and the model coefficients plus the intercept. No error is returned; callers are responsible for ensuring `features` has the same length as the training feature set.

```toke
m=example;
i=ml:std.ml;
i=linearmodel:std.ml;

f=example(model:$linearmodel):void{
  let pred = ml.linregpredict(model; @(4.0; 6.0));
};
```

### ml.kmeanstrain(data: @($row); k: u64; maxiter: u64): $kmeansmodel!$mlerr

Runs Lloyd's algorithm on `data` to produce a `$kmeansmodel` with `k` clusters, iterating at most `maxiter` times. Centroids are initialised to the first `k` data points, making results deterministic for the same input order. Returns `$mlerr` if `data` is empty or `k` exceeds the number of samples.

```toke
m=example;
i=ml:std.ml;

f=example():void{
  let rows = @(
    $row{vals:@(1.0; 1.0)};
    $row{vals:@(5.0; 5.0)};
    $row{vals:@(1.5; 1.5)};
    $row{vals:@(5.5; 5.2)}
  );
  let km = ml.kmeanstrain(rows; 2; 100)|{$ok:m m;$err:e $kmeansmodel{centroids:@();k:0}};
};
```

### ml.kmeansassign(model: $kmeansmodel; point: @(f64)): u64

Returns the index (0-based) of the centroid nearest to `point` using L2 distance. Use this function to classify new samples after training.

```toke
m=example;
i=ml:std.ml;
i=kmeansmodel:std.ml;

f=example(km:$kmeansmodel):void{
  let cluster = ml.kmeansassign(km; @(1.2; 1.3));
};
```

### ml.dtreefit(X: @($row); labels: @($str); maxdepth: u64): $dtreemodel!$mlerr

Trains a binary CART decision tree classifier on `X` with corresponding string class labels, growing the tree to at most `maxdepth` levels (root = depth 0). Returns `$mlerr` if `X` and `labels` differ in length or if the data is empty.

```toke
m=example;
i=ml:std.ml;

f=example():void{
  let ra = $row{vals:@(2.0; 3.0)};
  let rb = $row{vals:@(8.0; 7.0)};
  let rc = $row{vals:@(1.0; 2.0)};
  let rd = $row{vals:@(9.0; 8.0)};
  let feats = @(ra; rb; rc; rd);
  let labels = @("low"; "high"; "low"; "high");
  let tree = ml.dtreefit(feats; labels; 4)|{$ok:m m;$err:e $dtreemodel{}};
};
```

### ml.dtreepredict(model: $dtreemodel; features: @(f64)): $str

Walks the decision tree for a single query point and returns the majority class label of the leaf node reached. The label is always one of the strings seen during training.

```toke
m=example;
i=ml:std.ml;
i=dtreemodel:std.ml;

f=example(tree:$dtreemodel):void{
  let cls = ml.dtreepredict(tree; @(7.5; 6.8));
};
```

### ml.knnpredict(data: @($row); labels: @($str); point: @(f64); k: u64): $str

Performs brute-force k-nearest neighbours classification using L2 distance and returns the majority vote label among the `k` closest training points. Unlike `ml.dtreefit`/`ml.dtreepredict`, kNN requires no separate training step, making it convenient for small datasets.

```toke
m=example;
i=ml:std.ml;

f=example():void{
  let ra = $row{vals:@(2.0; 3.0)};
  let rb = $row{vals:@(8.0; 7.0)};
  let feats = @(ra; rb);
  let labs = @("low"; "high");
  let cls = ml.knnpredict(feats; labs; @(3.0; 4.0); 1);
};
```

## Complete Train → Predict Pipeline

This example shows a full linear regression workflow: fit a model, then predict on a new point.

```toke
m=example;
i=ml:std.ml;

f=train():i64{
  let r1=$row{vals:@(1.0)};
  let r2=$row{vals:@(2.0)};
  let r3=$row{vals:@(3.0)};
  let r4=$row{vals:@(4.0)};
  let traindata=@(r1;r2;r3;r4);
  let targets=@(2.0;4.0;6.0;8.0);
  let model=ml.linregfit(traindata;targets)|{$ok:m m;$err:e $linearmodel{coef:@();intercept:0.0}};
  let pred=ml.linregpredict(model;@(5.0));
  <0;
};

f=main():i64{
  < train()
};
```

## Evaluation Functions

### ml.accuracy(ytrue: @(u64); ypred: @(u64)): f64

Computes the fraction of correctly predicted labels over `n` samples, returning a value in `[0.0, 1.0]`.

```toke
m=example;
i=ml:std.ml;
f=demo():f64{
  let acc=ml.accuracy(@(1;0;1;1);@(1;0;0;1));
  < acc
};
```

## See Also

- `std.analytics` -- aggregations and descriptive statistics on tabular data.
- `std.dataframe` -- row-oriented data structures used as `$row` input.
- `std.math` -- numerical primitives shared with this module.
