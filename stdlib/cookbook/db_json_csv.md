# Cookbook: Data Export from Database

## Overview

Query a SQLite database, format the results as both JSON and CSV, and write each to a file. Combines `std.db`, `std.json`, and `std.csv` to demonstrate the common data export pattern where a single query feeds multiple output formats.

## Modules Used

- `std.db` -- SQLite database access and query execution
- `std.json` -- JSON object construction and serialization
- `std.csv` -- CSV row construction and file writing

## Complete Program

```toke
I std.db;
I std.json;
I std.csv;
I std.file;
I std.str;

(* Connect to the database *)
let conn = db.open("app.db")!;

(* Define the export query with a date filter *)
let start_date = "2025-01-01";
let end_date = "2025-12-31";

let rows = db.query(conn;
    "SELECT id, name, email, created_at, total_orders FROM customers WHERE created_at BETWEEN ? AND ? ORDER BY total_orders DESC";
    start_date;
    end_date;
)!;

let count = db.row_count(rows);

(* --- JSON export --- *)

let json_array = json.new_array();

for row in rows {
    let obj = json.new_object();
    json.set(obj; "id"; json.from_int(db.get_int(row; 0)));
    json.set(obj; "name"; json.from_str(db.get_text(row; 1)));
    json.set(obj; "email"; json.from_str(db.get_text(row; 2)));
    json.set(obj; "created_at"; json.from_str(db.get_text(row; 3)));
    json.set(obj; "total_orders"; json.from_int(db.get_int(row; 4)));
    json.push(json_array; obj);
};

(* Wrap in a metadata envelope *)
let envelope = json.new_object();
json.set(envelope; "export_date"; json.from_str("2026-04-19"));
json.set(envelope; "record_count"; json.from_int(count));
json.set(envelope; "date_range"; json.from_str(str.concat(start_date; str.concat(" to "; end_date))));
json.set(envelope; "data"; json_array);

let json_out = json.encode_pretty(envelope);
file.write_text("export/customers.json"; json_out)!;

(* --- CSV export --- *)

let csv_rows = csv.new_writer(csv.Options{
    header: true;
    delimiter: ',';
});

(* Write the header row *)
csv.write_header(csv_rows; @("id"; "name"; "email"; "created_at"; "total_orders"));

(* Write each data row *)
for row in rows {
    csv.write_row(csv_rows; @(
        str.from_int(db.get_int(row; 0));
        db.get_text(row; 1);
        db.get_text(row; 2);
        db.get_text(row; 3);
        str.from_int(db.get_int(row; 4));
    ));
};

let csv_out = csv.to_string(csv_rows);
file.write_text("export/customers.csv"; csv_out)!;

(* --- Summary --- *)

(* Compute total orders across all exported rows *)
let total = 0;
for row in rows {
    total = total + db.get_int(row; 4);
};

let summary = json.new_object();
json.set(summary; "records_exported"; json.from_int(count));
json.set(summary; "total_orders"; json.from_int(total));
json.set(summary; "json_file"; json.from_str("export/customers.json"));
json.set(summary; "csv_file"; json.from_str("export/customers.csv"));
file.write_text("export/summary.json"; json.encode_pretty(summary))!;

(* Close the database connection *)
db.close(conn);
```

## Step-by-Step Explanation

1. **Database query** -- `db.open` connects to SQLite. A parameterised query with `?` placeholders selects customers within a date range, preventing SQL injection.

2. **JSON export** -- Each row is converted to a JSON object. Fields are extracted by column index and wrapped with typed constructors (`json.from_int`, `json.from_str`). The array is wrapped in a metadata envelope.

3. **CSV export** -- `csv.new_writer` creates a CSV builder. The header row is written first, then each data row as a string array. `csv.to_string` produces the final CSV text.

4. **File output** -- Both formats are written to the `export/` directory using `file.write_text`. The JSON output uses `json.encode_pretty` for human readability.

5. **Summary report** -- A summary JSON file records how many records were exported and the total order count, useful for verification and auditing.

6. **Cleanup** -- `db.close` releases the database connection.
