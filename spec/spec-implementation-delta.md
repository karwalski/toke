# Spec-Implementation Delta: stdlib Modules

**Story:** 26.1.2
**Generated:** 2026-04-05

Tracks the gap between specified (.tki), implemented (.h/.c), and tested
(test_*.c) stdlib modules. Updated by auditing the filesystem directly.

## Status Key

| Status   | Meaning                                         |
|----------|-------------------------------------------------|
| complete | .tki + .h/.c + test_*.c all present             |
| partial  | some artefacts missing (usually test or spec)    |
| missing  | module absent from one or more stages            |

## Module Delta Table

| # | Module      | Specified (.tki) | Implemented (.h + .c) | Tested (test_*.c) | Status   | Notes |
|---|-------------|------------------|-----------------------|--------------------|----------|-------|
| 1 | analytics   | yes | yes | yes | complete | |
| 2 | auth        | yes | yes | yes | complete | Integration test also present |
| 3 | canvas      | yes | yes | yes | complete | |
| 4 | chart       | yes | yes | yes | complete | |
| 5 | crypto      | yes | yes | yes | complete | |
| 6 | csv         | yes | yes | yes | complete | |
| 7 | dashboard   | yes | yes | yes | complete | Integration test also present |
| 8 | dataframe   | yes | yes | yes | complete | |
| 9 | db          | yes | yes | yes | complete | |
| 10 | encoding   | yes | yes | yes | complete | |
| 11 | encrypt    | yes | yes | yes | complete | |
| 12 | env        | yes | yes | yes | complete | |
| 13 | file       | yes | yes | yes | complete | |
| 14 | html       | yes | yes | yes | complete | |
| 15 | http       | yes | yes | yes | complete | Integration test also present |
| 16 | i18n       | yes | yes | yes | complete | |
| 17 | image      | yes | yes | yes | complete | Integration test also present |
| 18 | json       | yes | yes | yes | complete | |
| 19 | llm        | yes | yes | yes | complete | Live test also present |
| 20 | llm_tool   | yes | yes | yes | complete | |
| 21 | log        | yes | yes | yes | complete | |
| 22 | math       | yes | yes | yes | complete | |
| 23 | ml         | yes | yes | yes | complete | Integration test also present |
| 24 | process    | yes | yes | yes | complete | |
| 25 | router     | yes | yes | yes | complete | |
| 26 | sse        | yes | yes | yes | complete | |
| 27 | str        | yes | yes | yes | complete | |
| 28 | svg        | yes | yes | yes | complete | Integration test also present |
| 29 | template   | yes | yes | yes | complete | Integration test also present |
| 30 | test       | yes | yes (tk_test.h/c) | yes (test_tktest.c) | complete | Impl uses `tk_test` prefix to avoid name collision |
| 31 | time       | yes | yes (tk_time.h/c) | yes (test_time.c) | complete | Impl uses `tk_time` prefix to avoid name collision |
| 32 | toon       | yes | yes | yes | complete | |
| 33 | ws         | yes | yes | yes | complete | Integration test also present |
| 34 | yaml       | yes | yes | yes | complete | |

## Summary

| Metric | Count |
|--------|-------|
| Total modules | 34 |
| Specified (.tki) | 34 |
| Implemented (.h + .c) | 34 |
| Tested (test_*.c) | 34 |
| Complete | 34 |
| Partial | 0 |
| Missing | 0 |

## Integration Tests

The following cross-module integration tests also exist in `test/stdlib/`:

| Test file | Modules covered |
|-----------|----------------|
| test_auth_flow_integration.c | auth, crypto, http |
| test_dashboard_integration.c | dashboard, chart, dataframe |
| test_data_pipeline.c | csv, dataframe, json |
| test_http_json_integration.c | http, json |
| test_image_pipeline_integration.c | image, svg, canvas |
| test_ml_pipeline_integration.c | ml, dataframe, math |
| test_network_integration.c | http, ws, sse |
| test_security_integration.c | crypto, encrypt, auth |
| test_svg_diagram_integration.c | svg, chart, canvas |
| test_template_email_integration.c | template, html |
| test_viz_integration.c | chart, svg, canvas |
| test_ws_handshake_integration.c | ws, http |

## Additional Coverage File

Detailed per-function conformance-test coverage (happy path + error variants)
is tracked separately in `docs/stdlib-coverage.md` (Story 3.8.3).

## Notes

- The `net` module has `.tk` and `.md` files in `stdlib/` but no `.tki` spec
  file and is not listed as a standalone module. Network functionality is
  provided through `std.http`, `std.ws`, and `std.sse`.
- `test_stdlib_coverage.c` is a cross-cutting coverage harness, not a
  per-module test.
- `bench_stdlib.c` is a benchmark file, not a test.
