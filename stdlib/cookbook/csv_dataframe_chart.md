# Cookbook: Data Visualization from CSV

## Overview

Load a CSV file into a dataframe, filter and group the data, then generate a bar chart image. Combines `std.csv`, `std.dataframe`, and `std.chart` to demonstrate the full data-to-visual pipeline in toke.

## Modules Used

- `std.csv` -- CSV file parsing and writing
- `std.dataframe` -- Tabular data manipulation
- `std.chart` -- Chart generation and rendering

## Complete Program

```toke
I std.csv;
I std.dataframe;
I std.chart;
I std.file;
I std.str;

(* Parse the CSV file into rows *)
let raw = csv.read("data/sales.csv"; csv.Options{
    header: true;
    delimiter: ',';
})!;

(* Convert CSV rows into a typed dataframe *)
let df = dataframe.from_csv(raw);

(* Filter to only 2025 records *)
let filtered = dataframe.filter(df; fn(row: dataframe.Row): Bool {
    let year = dataframe.get_int(row; "year");
    year == 2025;
});

(* Group by region and sum the revenue column *)
let grouped = dataframe.group_by(filtered; "region");
let summary = dataframe.agg(grouped; "revenue"; dataframe.Sum);

(* Sort descending by the aggregated revenue *)
let sorted = dataframe.sort(summary; "revenue"; dataframe.Desc);

(* Extract labels and values for the chart *)
let labels = dataframe.col_strs(sorted; "region");
let values = dataframe.col_floats(sorted; "revenue");

(* Build a bar chart *)
let ch = chart.new_bar(chart.BarConfig{
    title: "2025 Revenue by Region";
    x_label: "Region";
    y_label: "Revenue ($)";
    width: 800;
    height: 500;
    color: "#3b82f6";
});

chart.set_data(ch; labels; values);

(* Render the chart to a PNG file *)
chart.render_png(ch; "output/revenue_2025.png")!;

(* Also export the summary table as a new CSV *)
let out_rows = csv.from_dataframe(sorted);
csv.write("output/revenue_summary.csv"; out_rows; csv.Options{
    header: true;
    delimiter: ',';
})!;
```

## Example Input CSV

```
year,region,product,revenue
2024,North,Widget,12000
2025,North,Widget,15000
2025,South,Gadget,9200
2025,North,Gadget,7800
2025,West,Widget,11500
2025,South,Widget,6300
```

## Step-by-Step Explanation

1. **Parse CSV** -- `csv.read` loads the file and splits it into rows. The `header: true` option uses the first row as column names.

2. **Create dataframe** -- `dataframe.from_csv` converts the raw CSV rows into a typed columnar structure that supports filtering and aggregation.

3. **Filter** -- `dataframe.filter` keeps only rows where the callback returns true. Here it selects records from 2025.

4. **Group and aggregate** -- `dataframe.group_by` partitions rows by a column. `dataframe.agg` applies a summary function (Sum) to the revenue column within each group.

5. **Sort** -- `dataframe.sort` orders the aggregated results by revenue descending so the chart shows the highest region first.

6. **Build chart** -- `chart.new_bar` creates a bar chart with styling options. `chart.set_data` binds the label and value arrays.

7. **Render and export** -- `chart.render_png` writes the chart image. The summary is also written as a CSV for further use.
