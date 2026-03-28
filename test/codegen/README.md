# C-Series: Code Generation Tests

C-series conformance tests cover the LLVM IR backend (Stage 7) of tkc.

**Status:** Populated in Story 1.2.8 (LLVM IR backend implementation).

## Planned coverage

- Correct LLVM IR emitted for integer arithmetic
- Correct IR for boolean operations
- Struct allocation and field access IR
- Array allocation and bounds-check IR
- Loop and break IR structure
- Function call IR (including tail-call opportunities)
- Arena allocation scope IR (arena entry/exit)
- Return value passing conventions

## Notes

C-series tests require an executable tkc that can emit LLVM IR or a compiled
binary. Tests in this series use `expected_output` to verify the runtime result
of executing the compiled program. They depend on Story 1.2.8 (LLVM IR backend)
and Story 1.2.9 (CLI interface) being complete.

Tests will be added alongside Story 1.2.8 implementation work.
