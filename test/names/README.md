# N-Series: Name Resolution Tests

N-series conformance tests cover the name resolver (Stage 3) of tkc.

**Status:** Populated in Story 1.2.4 (Name resolver implementation).

## Planned coverage

- E3011: reserved built-in name used in binding position
- Undefined identifier reference
- Duplicate function/type/constant declaration at module scope
- Import alias shadowing
- `arena` used as user-defined identifier (E3011)
- Forward reference resolution for mutually recursive functions

## Error codes in scope

| Code | Condition |
|------|-----------|
| E3xxx | Name resolver errors (range 3000–3999) |

Tests will be added alongside Story 1.2.4 implementation work.
