#ifndef TK_TKIR_H
#define TK_TKIR_H

/*
 * tkir.h -- Binary IR (.tkir) encoder for toke.
 *
 * Emits a compact, version-stable, self-describing binary representation
 * of a toke programme.  The format is independent of LLVM and can be
 * lowered to any backend.  See docs/architecture/binary-ir-design.md.
 *
 * Story: 76.1.6a
 */

#include <stdint.h>
#include <stddef.h>
#include "parser.h"
#include "types.h"
#include "names.h"

/* ── File format constants ───────────────────────────────────────────── */

#define TKIR_MAGIC          0x544B4952u  /* "TKIR" in ASCII, little-endian */
#define TKIR_VERSION_MAJOR  1
#define TKIR_VERSION_MINOR  0

/* ── Section type IDs (matching binary-ir-design.md) ─────────────────── */

#define TKIR_SEC_TYPE       0x01
#define TKIR_SEC_FUNC       0x02
#define TKIR_SEC_CODE       0x03
#define TKIR_SEC_DATA       0x04
#define TKIR_SEC_IMPORT     0x05
#define TKIR_SEC_EXPORT     0x06

/* ── Type entry kinds ────────────────────────────────────────────────── */

#define TKIR_TY_I64         0x01
#define TKIR_TY_F64         0x02
#define TKIR_TY_BOOL        0x03
#define TKIR_TY_STR         0x04
#define TKIR_TY_VOID        0x05
#define TKIR_TY_STRUCT      0x06
#define TKIR_TY_ARRAY       0x07
#define TKIR_TY_FN_PTR      0x08
#define TKIR_TY_ERR_UNION   0x09

/* ── Opcodes ─────────────────────────────────────────────────────────── */

/* Arithmetic */
#define TKIR_OP_ADD         0x01
#define TKIR_OP_SUB         0x02
#define TKIR_OP_MUL         0x03
#define TKIR_OP_DIV         0x04
#define TKIR_OP_MOD         0x05
#define TKIR_OP_NEG         0x06

/* Comparison */
#define TKIR_OP_EQ          0x10
#define TKIR_OP_NE          0x11
#define TKIR_OP_LT          0x12
#define TKIR_OP_LE          0x13
#define TKIR_OP_GT          0x14
#define TKIR_OP_GE          0x15

/* Control flow */
#define TKIR_OP_BR          0x20
#define TKIR_OP_BR_IF       0x21
#define TKIR_OP_CALL        0x22
#define TKIR_OP_RET         0x23

/* Memory */
#define TKIR_OP_ALLOCA      0x30
#define TKIR_OP_LOAD        0x31
#define TKIR_OP_STORE       0x32
#define TKIR_OP_CONST_DATA  0x33

/* Type operations */
#define TKIR_OP_CAST        0x40
#define TKIR_OP_FIELD_GET   0x41
#define TKIR_OP_FIELD_SET   0x42

/* ── Data entry kinds ────────────────────────────────────────────────── */

#define TKIR_DATA_STR       0x01
#define TKIR_DATA_I64       0x02
#define TKIR_DATA_F64       0x03

/* ── Function flags ──────────────────────────────────────────────────── */

#define TKIR_FN_EXPORTED    0x01
#define TKIR_FN_EXTERN      0x02

/* ── Header flags ────────────────────────────────────────────────────── */

#define TKIR_FLAG_DEBUG      0x01
#define TKIR_FLAG_EXPORTS    0x02

/* ── Error codes ─────────────────────────────────────────────────────── */

#define E9020 9020  /* failed to write .tkir file */
#define E9021 9021  /* failed to read .tkir file  */

/* ── Encoder buffer ──────────────────────────────────────────────────── */

/*
 * TkirBuf -- Growable byte buffer used to accumulate binary output.
 * Callers should not access fields directly; use tkir_buf_* helpers.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} TkirBuf;

void tkir_buf_init(TkirBuf *b);
void tkir_buf_free(TkirBuf *b);
void tkir_buf_u8(TkirBuf *b, uint8_t v);
void tkir_buf_u16(TkirBuf *b, uint16_t v);
void tkir_buf_u32(TkirBuf *b, uint32_t v);
void tkir_buf_i64(TkirBuf *b, int64_t v);
void tkir_buf_bytes(TkirBuf *b, const void *src, size_t n);
void tkir_buf_str(TkirBuf *b, const char *s, size_t len);

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * emit_tkir -- Walk the type-checked AST and write a .tkir binary to
 * out_path.  Returns 0 on success, -1 on error (E9020 emitted).
 *
 * ast      : root NODE_PROGRAM from the parser
 * src      : original source text
 * types    : completed type environment
 * names    : completed name environment
 * target   : LLVM-style target triple, or NULL for "native"
 * out_path : file path for the .tkir output
 */
int emit_tkir(const Node *ast, const char *src,
              const TypeEnv *types, const NameEnv *names,
              const char *target, const char *out_path);

/* ── Reader: in-memory structures ────────────────────────────────────── */

/*
 * Story: 76.1.6b — Binary IR reader.
 *
 * These structures represent a decoded .tkir file in memory.  They are
 * independent of the compiler's AST/TypeEnv so that .tkir files can be
 * loaded without the full compiler pipeline.
 */

/* Maximum valid opcode value (for validation) */
#define TKIR_OP_MAX  0x42

/*
 * TkirInstr — a single decoded instruction.
 */
typedef struct {
    uint8_t  opcode;
    uint16_t dst;       /* destination register                          */
    uint16_t src;       /* source / lhs register                         */
    uint16_t rhs;       /* second source register (binary ops)           */
    union {
        int64_t  i64;
        double   f64;
        uint32_t u32;
    } imm;
    /* CALL operands */
    uint16_t  arg_count;
    uint16_t *arg_regs;  /* heap-allocated, NULL when arg_count == 0     */
} TkirInstr;

/*
 * TkirTypeEntry — an entry from the type section.
 */
typedef struct {
    uint8_t  kind;           /* TKIR_TY_* constant                       */
    char     name[128];      /* struct name, empty for primitives         */
    uint16_t elem_type_idx;  /* element/return type index                 */
    uint16_t field_count;
    uint16_t *field_type_idxs; /* heap-allocated                          */
} TkirTypeEntry;

/*
 * TkirFuncDef — a decoded function with its instruction stream.
 */
typedef struct {
    char     name[128];
    uint8_t  flags;          /* TKIR_FN_EXPORTED, TKIR_FN_EXTERN         */
    uint16_t type_idx;       /* index into TkirModule.types               */
    uint16_t reg_count;      /* total registers used                      */
    uint16_t param_count;
    int      instr_count;
    TkirInstr *instrs;       /* heap-allocated                            */
} TkirFuncDef;

/*
 * TkirDataEntry — a data/constant entry.
 */
typedef struct {
    uint8_t  kind;           /* TKIR_DATA_STR / I64 / F64                */
    uint32_t size;
    uint8_t *bytes;          /* heap-allocated raw payload                */
} TkirDataEntry;

/*
 * TkirImportEntry — an imported symbol.
 */
typedef struct {
    char     name[128];
    char     module[256];
    uint8_t  kind;           /* 0=func, 1=data                           */
    uint16_t type_idx;
} TkirImportEntry;

/*
 * TkirExportEntry — an exported symbol.
 */
typedef struct {
    char     name[128];
    uint8_t  kind;           /* 0=func, 1=data                           */
    uint16_t index;
} TkirExportEntry;

/*
 * TkirModule — top-level container for a decoded .tkir file.
 *
 * Populated by tkir_read() / tkir_decode().  Free with tkir_module_free().
 */
typedef struct {
    char module_name[256];
    char target[64];

    int            type_count;
    TkirTypeEntry *types;

    int            func_count;
    TkirFuncDef   *funcs;

    int            data_count;
    TkirDataEntry *data;

    int              import_count;
    TkirImportEntry *imports;

    int              export_count;
    TkirExportEntry *exports;

    uint8_t version_major;
    uint8_t version_minor;
    uint8_t header_flags;
} TkirModule;

/* ── Reader API ──────────────────────────────────────────────────────── */

/*
 * tkir_read — read a .tkir file from disk into a TkirModule.
 *
 * Validates magic bytes, version, section structure, opcode values,
 * and register references.
 * Returns 0 on success, -1 on error.  If errbuf is non-NULL, a
 * human-readable error message is written there.
 */
int tkir_read(const char *path, TkirModule *out,
              char *errbuf, int errbuf_len);

/*
 * tkir_decode — parse a .tkir binary from a memory buffer.
 *
 * Same validation as tkir_read but operates on an in-memory buffer.
 * Returns 0 on success, -1 on error.
 */
int tkir_decode(const uint8_t *buf, size_t len, TkirModule *out,
                char *errbuf, int errbuf_len);

/*
 * tkir_dump — print a human-readable summary of a TkirModule to f.
 *
 * Used by the --read-tkir CLI flag.
 */
void tkir_dump(const TkirModule *mod, FILE *f);

/*
 * tkir_module_free — release all heap memory owned by a TkirModule.
 *
 * Does not free the TkirModule struct itself (it may be stack-allocated).
 */
void tkir_module_free(TkirModule *mod);

/*
 * tkir_opcode_name — return the human-readable name for an opcode.
 * Returns "UNKNOWN" for out-of-range values.
 */
const char *tkir_opcode_name(uint8_t opcode);

/*
 * tkir_type_kind_name — return the human-readable name for a type kind.
 * Returns "UNKNOWN" for out-of-range values.
 */
const char *tkir_type_kind_name(uint8_t kind);

#endif /* TK_TKIR_H */
