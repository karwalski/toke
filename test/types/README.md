# T-Series: Type Checking Tests

T-series conformance tests cover the type checker (Stage 4) of tkc.

**Status:** Populated in Story 1.2.5 (Type checker implementation).

## Planned coverage

- Scalar type compatibility (all 10 numeric types + bool + Str + Byte)
- Return type mismatch
- Struct field type mismatch
- Sum type exhaustive match enforcement (E4010)
- Mixing struct fields and sum variant tags in one TypeDecl (E2010)
- Cast expression validity (as TypeExpr)
- Error propagation postfix (! TypeExpr) against declared error type
- Array element type homogeneity
- Function call argument type matching

## Error codes in scope

| Code | Condition |
|------|-----------|
| E4xxx | Type checker errors (range 4000–4999) |

Tests will be added alongside Story 1.2.5 implementation work.
