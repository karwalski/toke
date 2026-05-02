/*
 * test_tkir_reader.c — Unit tests for the .tkir binary format reader.
 *
 * Constructs .tkir binaries in memory using the TkirBuf encoder helpers,
 * then reads them back with tkir_decode() and verifies the round-trip.
 *
 * Story: 76.1.6b
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../src/tkir.h"

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stderr, "  TEST %s ... ", #name); \
    name(); \
    tests_passed++; \
    fprintf(stderr, "PASS\n"); \
} while (0)

/* Write a length-prefixed string into a TkirBuf (u16 len + bytes). */
static void buf_write_str(TkirBuf *b, const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    tkir_buf_u16(b, len);
    tkir_buf_bytes(b, s, len);
}

/* Build a minimal valid header with 0 sections. */
static void write_header(TkirBuf *b, uint16_t sec_count)
{
    tkir_buf_u32(b, TKIR_MAGIC);
    tkir_buf_u8(b, TKIR_VERSION_MAJOR);
    tkir_buf_u8(b, TKIR_VERSION_MINOR);
    tkir_buf_u8(b, 0);  /* flags */
    tkir_buf_u16(b, sec_count);
}

/* ── Tests ───────────────���───────────────────────────��───────────────── */

/* Test 1: Minimal valid header with no sections. */
static void test_empty_module(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);
    write_header(&buf, 0);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == 0);
    assert(mod.type_count == 0);
    assert(mod.func_count == 0);
    assert(mod.data_count == 0);
    assert(mod.import_count == 0);
    assert(mod.export_count == 0);
    assert(mod.version_major == TKIR_VERSION_MAJOR);
    assert(mod.version_minor == TKIR_VERSION_MINOR);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 2: Bad magic is rejected. */
static void test_bad_magic(void)
{
    uint8_t data[] = { 'B', 'A', 'D', '!', 1, 0, 0, 0, 0 };
    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(data, sizeof(data), &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == -1);
    assert(strstr(errbuf, "magic") != NULL);
}

/* Test 3: Unsupported version is rejected. */
static void test_bad_version(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);
    tkir_buf_u32(&buf, TKIR_MAGIC);
    tkir_buf_u8(&buf, 99);  /* bad major version */
    tkir_buf_u8(&buf, 0);
    tkir_buf_u8(&buf, 0);
    tkir_buf_u16(&buf, 0);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == -1);
    assert(strstr(errbuf, "version") != NULL);

    tkir_buf_free(&buf);
}

/* Test 4: Truncated file is rejected. */
static void test_truncated(void)
{
    uint8_t data[] = { 'T', 'K', 'I', 'R', 1 };  /* only 5 bytes */
    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(data, sizeof(data), &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == -1);
}

/* Test 5: Section length exceeding file size is rejected. */
static void test_section_overflow(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);
    write_header(&buf, 1);
    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, 99999);  /* way too large */

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == -1);
    assert(strstr(errbuf, "exceeds") != NULL);

    tkir_buf_free(&buf);
}

/* Test 6: Type section round-trip. */
static void test_type_section(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    /* Build type section payload first to know its length */
    TkirBuf sec;
    tkir_buf_init(&sec);

    tkir_buf_u16(&sec, 3);  /* 3 types */

    /* Type 0: i64 (primitive) */
    tkir_buf_u8(&sec, TKIR_TY_I64);

    /* Type 1: str (primitive) */
    tkir_buf_u8(&sec, TKIR_TY_STR);

    /* Type 2: struct "Point" with 2 fields of type i64 */
    tkir_buf_u8(&sec, TKIR_TY_STRUCT);
    buf_write_str(&sec, "Point");
    tkir_buf_u16(&sec, 2);  /* field count */
    tkir_buf_u16(&sec, 0);  /* field 0: type idx 0 (i64) */
    tkir_buf_u16(&sec, 0);  /* field 1: type idx 0 (i64) */

    /* Write the full file */
    write_header(&buf, 1);
    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, (uint32_t)sec.len);
    tkir_buf_bytes(&buf, sec.data, sec.len);
    tkir_buf_free(&sec);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == 0);
    assert(mod.type_count == 3);

    assert(mod.types[0].kind == TKIR_TY_I64);
    assert(mod.types[1].kind == TKIR_TY_STR);
    assert(mod.types[2].kind == TKIR_TY_STRUCT);
    assert(strcmp(mod.types[2].name, "Point") == 0);
    assert(mod.types[2].field_count == 2);
    assert(mod.types[2].field_type_idxs[0] == 0);
    assert(mod.types[2].field_type_idxs[1] == 0);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 7: Func + code section round-trip. */
static void test_func_and_code(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    /* Type section: 1 type (fn ptr) */
    TkirBuf tsec;
    tkir_buf_init(&tsec);
    tkir_buf_u16(&tsec, 1);
    tkir_buf_u8(&tsec, TKIR_TY_FN_PTR);
    buf_write_str(&tsec, "add");
    tkir_buf_u16(&tsec, 0);   /* elem (return type) — idx 0 self-ref for stub */
    tkir_buf_u16(&tsec, 0);   /* param count */

    /* Func section: 1 function */
    TkirBuf fsec;
    tkir_buf_init(&fsec);
    tkir_buf_u16(&fsec, 1);  /* count */
    buf_write_str(&fsec, "add");
    tkir_buf_u8(&fsec, TKIR_FN_EXPORTED);  /* flags */
    tkir_buf_u16(&fsec, 0);   /* type_idx */
    tkir_buf_u16(&fsec, 4);   /* reg_count */
    tkir_buf_u16(&fsec, 2);   /* param_count */

    /* Code section: 1 func with 2 instructions */
    TkirBuf csec;
    tkir_buf_init(&csec);
    tkir_buf_u16(&csec, 0);   /* func_idx */
    tkir_buf_u16(&csec, 2);   /* instr_count */

    /* Instr 0: ADD r2 = r0 + r1 */
    tkir_buf_u8(&csec, TKIR_OP_ADD);
    tkir_buf_u16(&csec, 2);   /* dst */
    tkir_buf_u16(&csec, 0);   /* src/lhs */
    tkir_buf_u16(&csec, 1);   /* rhs */

    /* Instr 1: RET r2 */
    tkir_buf_u8(&csec, TKIR_OP_RET);
    tkir_buf_u16(&csec, 2);   /* dst (unused for ret, but present in encoding) */
    tkir_buf_u16(&csec, 2);   /* src (return value) */
    tkir_buf_u16(&csec, 0);   /* rhs (unused) */

    /* Assemble full file: 3 sections */
    write_header(&buf, 3);

    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, (uint32_t)tsec.len);
    tkir_buf_bytes(&buf, tsec.data, tsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_FUNC);
    tkir_buf_u32(&buf, (uint32_t)fsec.len);
    tkir_buf_bytes(&buf, fsec.data, fsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_CODE);
    tkir_buf_u32(&buf, (uint32_t)csec.len);
    tkir_buf_bytes(&buf, csec.data, csec.len);

    tkir_buf_free(&tsec);
    tkir_buf_free(&fsec);
    tkir_buf_free(&csec);

    /* Decode */
    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    if (rc != 0) fprintf(stderr, "ERROR: %s\n", errbuf);
    assert(rc == 0);

    assert(mod.func_count == 1);
    assert(strcmp(mod.funcs[0].name, "add") == 0);
    assert(mod.funcs[0].flags == TKIR_FN_EXPORTED);
    assert(mod.funcs[0].reg_count == 4);
    assert(mod.funcs[0].param_count == 2);
    assert(mod.funcs[0].instr_count == 2);

    assert(mod.funcs[0].instrs[0].opcode == TKIR_OP_ADD);
    assert(mod.funcs[0].instrs[0].dst == 2);
    assert(mod.funcs[0].instrs[0].src == 0);
    assert(mod.funcs[0].instrs[0].rhs == 1);

    assert(mod.funcs[0].instrs[1].opcode == TKIR_OP_RET);
    assert(mod.funcs[0].instrs[1].src == 2);

    /* Verify opcode names */
    assert(strcmp(tkir_opcode_name(TKIR_OP_ADD), "ADD") == 0);
    assert(strcmp(tkir_opcode_name(TKIR_OP_RET), "RET") == 0);
    assert(strcmp(tkir_opcode_name(0xFF), "UNKNOWN") == 0);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 8: Data section round-trip. */
static void test_data_section(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    TkirBuf dsec;
    tkir_buf_init(&dsec);
    tkir_buf_u16(&dsec, 2);  /* count */

    /* Entry 0: string "hello" */
    tkir_buf_u8(&dsec, TKIR_DATA_STR);
    tkir_buf_u32(&dsec, 5);
    tkir_buf_bytes(&dsec, "hello", 5);

    /* Entry 1: i64 value (8 bytes) */
    tkir_buf_u8(&dsec, TKIR_DATA_I64);
    tkir_buf_u32(&dsec, 8);
    uint8_t i64_bytes[8] = { 42, 0, 0, 0, 0, 0, 0, 0 };
    tkir_buf_bytes(&dsec, i64_bytes, 8);

    write_header(&buf, 1);
    tkir_buf_u8(&buf, TKIR_SEC_DATA);
    tkir_buf_u32(&buf, (uint32_t)dsec.len);
    tkir_buf_bytes(&buf, dsec.data, dsec.len);
    tkir_buf_free(&dsec);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == 0);
    assert(mod.data_count == 2);
    assert(mod.data[0].kind == TKIR_DATA_STR);
    assert(mod.data[0].size == 5);
    assert(memcmp(mod.data[0].bytes, "hello", 5) == 0);
    assert(mod.data[1].kind == TKIR_DATA_I64);
    assert(mod.data[1].size == 8);
    assert(mod.data[1].bytes[0] == 42);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 9: Import and export sections. */
static void test_import_export(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    /* Import section */
    TkirBuf isec;
    tkir_buf_init(&isec);
    tkir_buf_u16(&isec, 1);
    buf_write_str(&isec, "println");
    buf_write_str(&isec, "std.io");
    tkir_buf_u8(&isec, 0);   /* kind = func */
    tkir_buf_u16(&isec, 0);  /* type_idx */

    /* Export section */
    TkirBuf esec;
    tkir_buf_init(&esec);
    tkir_buf_u16(&esec, 1);
    buf_write_str(&esec, "main");
    tkir_buf_u8(&esec, 0);   /* kind = func */
    tkir_buf_u16(&esec, 0);  /* index */

    write_header(&buf, 2);

    tkir_buf_u8(&buf, TKIR_SEC_IMPORT);
    tkir_buf_u32(&buf, (uint32_t)isec.len);
    tkir_buf_bytes(&buf, isec.data, isec.len);

    tkir_buf_u8(&buf, TKIR_SEC_EXPORT);
    tkir_buf_u32(&buf, (uint32_t)esec.len);
    tkir_buf_bytes(&buf, esec.data, esec.len);

    tkir_buf_free(&isec);
    tkir_buf_free(&esec);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == 0);

    assert(mod.import_count == 1);
    assert(strcmp(mod.imports[0].name, "println") == 0);
    assert(strcmp(mod.imports[0].module, "std.io") == 0);
    assert(mod.imports[0].kind == 0);

    assert(mod.export_count == 1);
    assert(strcmp(mod.exports[0].name, "main") == 0);
    assert(mod.exports[0].kind == 0);
    assert(mod.exports[0].index == 0);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 10: Invalid opcode in code section is rejected. */
static void test_invalid_opcode(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    /* Need type + func + code sections for opcode validation */
    TkirBuf tsec;
    tkir_buf_init(&tsec);
    tkir_buf_u16(&tsec, 1);
    tkir_buf_u8(&tsec, TKIR_TY_I64);

    TkirBuf fsec;
    tkir_buf_init(&fsec);
    tkir_buf_u16(&fsec, 1);
    buf_write_str(&fsec, "bad");
    tkir_buf_u8(&fsec, 0);
    tkir_buf_u16(&fsec, 0);
    tkir_buf_u16(&fsec, 4);
    tkir_buf_u16(&fsec, 0);

    TkirBuf csec;
    tkir_buf_init(&csec);
    tkir_buf_u16(&csec, 0);   /* func_idx */
    tkir_buf_u16(&csec, 1);   /* instr_count */
    tkir_buf_u8(&csec, 0xFF); /* INVALID opcode */
    tkir_buf_u16(&csec, 0);
    tkir_buf_u16(&csec, 0);
    tkir_buf_u16(&csec, 0);

    write_header(&buf, 3);

    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, (uint32_t)tsec.len);
    tkir_buf_bytes(&buf, tsec.data, tsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_FUNC);
    tkir_buf_u32(&buf, (uint32_t)fsec.len);
    tkir_buf_bytes(&buf, fsec.data, fsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_CODE);
    tkir_buf_u32(&buf, (uint32_t)csec.len);
    tkir_buf_bytes(&buf, csec.data, csec.len);

    tkir_buf_free(&tsec);
    tkir_buf_free(&fsec);
    tkir_buf_free(&csec);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == -1);
    assert(strstr(errbuf, "invalid opcode") != NULL);

    tkir_buf_free(&buf);
}

/* Test 11: Register reference exceeding reg_count is rejected. */
static void test_register_overflow(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    TkirBuf tsec;
    tkir_buf_init(&tsec);
    tkir_buf_u16(&tsec, 1);
    tkir_buf_u8(&tsec, TKIR_TY_I64);

    TkirBuf fsec;
    tkir_buf_init(&fsec);
    tkir_buf_u16(&fsec, 1);
    buf_write_str(&fsec, "over");
    tkir_buf_u8(&fsec, 0);
    tkir_buf_u16(&fsec, 0);
    tkir_buf_u16(&fsec, 2);   /* reg_count = 2, so regs 0-1 valid */
    tkir_buf_u16(&fsec, 0);

    TkirBuf csec;
    tkir_buf_init(&csec);
    tkir_buf_u16(&csec, 0);   /* func_idx */
    tkir_buf_u16(&csec, 1);   /* instr_count */
    tkir_buf_u8(&csec, TKIR_OP_ADD);
    tkir_buf_u16(&csec, 5);   /* dst=5, out of range */
    tkir_buf_u16(&csec, 0);
    tkir_buf_u16(&csec, 1);

    write_header(&buf, 3);

    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, (uint32_t)tsec.len);
    tkir_buf_bytes(&buf, tsec.data, tsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_FUNC);
    tkir_buf_u32(&buf, (uint32_t)fsec.len);
    tkir_buf_bytes(&buf, fsec.data, fsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_CODE);
    tkir_buf_u32(&buf, (uint32_t)csec.len);
    tkir_buf_bytes(&buf, csec.data, csec.len);

    tkir_buf_free(&tsec);
    tkir_buf_free(&fsec);
    tkir_buf_free(&csec);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == -1);
    assert(strstr(errbuf, "register") != NULL);

    tkir_buf_free(&buf);
}

/* Test 12: File-based round-trip via tkir_read. */
static void test_file_roundtrip(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    /* Build a module with type + func + code + export */
    TkirBuf tsec;
    tkir_buf_init(&tsec);
    tkir_buf_u16(&tsec, 1);
    tkir_buf_u8(&tsec, TKIR_TY_FN_PTR);
    buf_write_str(&tsec, "identity");
    tkir_buf_u16(&tsec, 0);
    tkir_buf_u16(&tsec, 0);

    TkirBuf fsec;
    tkir_buf_init(&fsec);
    tkir_buf_u16(&fsec, 1);
    buf_write_str(&fsec, "identity");
    tkir_buf_u8(&fsec, TKIR_FN_EXPORTED);
    tkir_buf_u16(&fsec, 0);
    tkir_buf_u16(&fsec, 2);
    tkir_buf_u16(&fsec, 1);

    TkirBuf csec;
    tkir_buf_init(&csec);
    tkir_buf_u16(&csec, 0);
    tkir_buf_u16(&csec, 1);
    tkir_buf_u8(&csec, TKIR_OP_RET);
    tkir_buf_u16(&csec, 0);
    tkir_buf_u16(&csec, 0);
    tkir_buf_u16(&csec, 0);

    TkirBuf esec;
    tkir_buf_init(&esec);
    tkir_buf_u16(&esec, 1);
    buf_write_str(&esec, "identity");
    tkir_buf_u8(&esec, 0);
    tkir_buf_u16(&esec, 0);

    write_header(&buf, 4);

    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, (uint32_t)tsec.len);
    tkir_buf_bytes(&buf, tsec.data, tsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_FUNC);
    tkir_buf_u32(&buf, (uint32_t)fsec.len);
    tkir_buf_bytes(&buf, fsec.data, fsec.len);

    tkir_buf_u8(&buf, TKIR_SEC_CODE);
    tkir_buf_u32(&buf, (uint32_t)csec.len);
    tkir_buf_bytes(&buf, csec.data, csec.len);

    tkir_buf_u8(&buf, TKIR_SEC_EXPORT);
    tkir_buf_u32(&buf, (uint32_t)esec.len);
    tkir_buf_bytes(&buf, esec.data, esec.len);

    tkir_buf_free(&tsec);
    tkir_buf_free(&fsec);
    tkir_buf_free(&csec);
    tkir_buf_free(&esec);

    /* Write to temp file */
    const char *path = "/tmp/test_tkir_roundtrip.tkir";
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fwrite(buf.data, 1, buf.len, f);
    fclose(f);
    tkir_buf_free(&buf);

    /* Read back */
    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_read(path, &mod, errbuf, (int)sizeof(errbuf));
    if (rc != 0) fprintf(stderr, "ERROR: %s\n", errbuf);
    assert(rc == 0);

    assert(mod.type_count == 1);
    assert(mod.func_count == 1);
    assert(strcmp(mod.funcs[0].name, "identity") == 0);
    assert(mod.funcs[0].instr_count == 1);
    assert(mod.funcs[0].instrs[0].opcode == TKIR_OP_RET);
    assert(mod.export_count == 1);
    assert(strcmp(mod.exports[0].name, "identity") == 0);

    tkir_module_free(&mod);

    /* Clean up temp file */
    remove(path);
}

/* Test 13: tkir_dump produces output without crashing. */
static void test_dump(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    TkirBuf tsec;
    tkir_buf_init(&tsec);
    tkir_buf_u16(&tsec, 1);
    tkir_buf_u8(&tsec, TKIR_TY_I64);

    write_header(&buf, 1);
    tkir_buf_u8(&buf, TKIR_SEC_TYPE);
    tkir_buf_u32(&buf, (uint32_t)tsec.len);
    tkir_buf_bytes(&buf, tsec.data, tsec.len);
    tkir_buf_free(&tsec);

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == 0);

    /* Dump to /dev/null — just verify it doesn't crash */
    FILE *devnull = fopen("/dev/null", "w");
    assert(devnull != NULL);
    tkir_dump(&mod, devnull);
    fclose(devnull);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 14: Unknown sections are skipped (forward compat). */
static void test_unknown_section_skipped(void)
{
    TkirBuf buf;
    tkir_buf_init(&buf);

    write_header(&buf, 1);
    tkir_buf_u8(&buf, 0xFE);  /* unknown section tag */
    tkir_buf_u32(&buf, 4);    /* 4 bytes payload */
    tkir_buf_u32(&buf, 0);    /* dummy payload */

    TkirModule mod;
    char errbuf[256] = {0};
    int rc = tkir_decode(buf.data, buf.len, &mod, errbuf, (int)sizeof(errbuf));
    assert(rc == 0);  /* should succeed, unknown section skipped */
    assert(mod.type_count == 0);

    tkir_module_free(&mod);
    tkir_buf_free(&buf);
}

/* Test 15: Type kind name helper. */
static void test_type_kind_names(void)
{
    assert(strcmp(tkir_type_kind_name(TKIR_TY_I64), "i64") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_F64), "f64") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_BOOL), "bool") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_STR), "str") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_VOID), "void") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_STRUCT), "struct") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_ARRAY), "array") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_FN_PTR), "fn") == 0);
    assert(strcmp(tkir_type_kind_name(TKIR_TY_ERR_UNION), "error_union") == 0);
    assert(strcmp(tkir_type_kind_name(0xFF), "UNKNOWN") == 0);
}

/* ── Main ───────────��────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stderr, "=== TKIR Reader Tests ===\n");

    TEST(test_empty_module);
    TEST(test_bad_magic);
    TEST(test_bad_version);
    TEST(test_truncated);
    TEST(test_section_overflow);
    TEST(test_type_section);
    TEST(test_func_and_code);
    TEST(test_data_section);
    TEST(test_import_export);
    TEST(test_invalid_opcode);
    TEST(test_register_overflow);
    TEST(test_file_roundtrip);
    TEST(test_dump);
    TEST(test_unknown_section_skipped);
    TEST(test_type_kind_names);

    fprintf(stderr, "\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
