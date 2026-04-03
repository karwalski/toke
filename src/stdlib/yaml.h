#ifndef TK_STDLIB_YAML_H
#define TK_STDLIB_YAML_H

/*
 * yaml.h — C interface for the std.yaml standard library module.
 *
 * Minimal YAML parser/emitter for toke. Handles flat key-value mappings
 * and simple sequences. No anchors, aliases, or multi-document streams.
 *
 * Type mappings:
 *   Yaml           = struct { const char *raw; }
 *   YamlErr        = struct { YamlErrKind kind; const char *msg; }
 *   Yaml!YamlErr   = YamlResult
 *
 * Story: 6.3.3
 */

#include <stdint.h>

typedef struct { const char *raw; } Yaml;
typedef struct { Yaml *data; uint64_t len; } YamlArray;

typedef enum {
    YAML_ERR_PARSE   = 0,
    YAML_ERR_TYPE    = 1,
    YAML_ERR_MISSING = 2
} YamlErrKind;

typedef struct { YamlErrKind kind; const char *msg; } YamlErr;

typedef struct { Yaml       ok; int is_err; YamlErr err; } YamlResult;
typedef struct { const char *ok; int is_err; YamlErr err; } StrYamlResult;
typedef struct { int64_t    ok; int is_err; YamlErr err; } I64YamlResult;
typedef struct { double     ok; int is_err; YamlErr err; } F64YamlResult;
typedef struct { int        ok; int is_err; YamlErr err; } BoolYamlResult;
typedef struct { YamlArray  ok; int is_err; YamlErr err; } YamlArrayResult;

/* Core API */
const char      *yaml_enc(const char *v);
YamlResult       yaml_dec(const char *s);
StrYamlResult    yaml_str(Yaml y, const char *key);
I64YamlResult    yaml_i64(Yaml y, const char *key);
F64YamlResult    yaml_f64(Yaml y, const char *key);
BoolYamlResult   yaml_bool(Yaml y, const char *key);
YamlArrayResult  yaml_arr(Yaml y, const char *key);

/* Conversion */
const char *yaml_from_json(const char *json);
const char *yaml_to_json(const char *yaml);

#endif /* TK_STDLIB_YAML_H */
