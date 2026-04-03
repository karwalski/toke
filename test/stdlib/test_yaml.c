/*
 * test_yaml.c — Unit tests for std.yaml (yaml.h / yaml.c).
 *
 * Story: 6.3.3
 *
 * Build: cc -I../../src/stdlib test_yaml.c ../../src/stdlib/yaml.c -o test_yaml
 */

#include "yaml.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, label) \
    do { if (cond) { printf("PASS  %s\n", label); g_pass++; } \
         else      { printf("FAIL  %s\n", label); g_fail++; } } while(0)

int main(void) {
    /* --- yaml_dec success --- */
    YamlResult dr = yaml_dec("name: Alice\nage: 30\nactive: true\n");
    CHECK(!dr.is_err, "yaml_dec valid YAML");
    Yaml y = dr.ok;

    /* --- yaml_str success --- */
    StrYamlResult sr = yaml_str(y, "name");
    CHECK(!sr.is_err && strcmp(sr.ok, "Alice") == 0, "yaml_str key=name");

    /* --- yaml_i64 success --- */
    I64YamlResult ir = yaml_i64(y, "age");
    CHECK(!ir.is_err && ir.ok == 30, "yaml_i64 key=age");

    /* --- yaml_bool success --- */
    BoolYamlResult br = yaml_bool(y, "active");
    CHECK(!br.is_err && br.ok == 1, "yaml_bool key=active (true)");

    /* --- yaml_str missing key --- */
    StrYamlResult mr = yaml_str(y, "missing");
    CHECK(mr.is_err && mr.err.kind == YAML_ERR_MISSING, "yaml_str missing key");

    /* --- yaml_dec empty --- */
    YamlResult empty = yaml_dec("");
    CHECK(empty.is_err, "yaml_dec empty input");

    /* --- yaml_bool yes/no --- */
    YamlResult yn = yaml_dec("enabled: yes\ndisabled: no\n");
    CHECK(!yn.is_err, "yaml_dec yes/no booleans");
    BoolYamlResult yes_r = yaml_bool(yn.ok, "enabled");
    CHECK(!yes_r.is_err && yes_r.ok == 1, "yaml_bool yes=true");
    BoolYamlResult no_r = yaml_bool(yn.ok, "disabled");
    CHECK(!no_r.is_err && no_r.ok == 0, "yaml_bool no=false");

    /* --- yaml_f64 --- */
    YamlResult fd = yaml_dec("pi: 3.14\ne: 2.71\n");
    CHECK(!fd.is_err, "yaml_dec float data");
    F64YamlResult fr = yaml_f64(fd.ok, "pi");
    CHECK(!fr.is_err && fr.ok > 3.13 && fr.ok < 3.15, "yaml_f64 key=pi");

    /* --- yaml_arr sequence --- */
    YamlResult ad = yaml_dec("items:\n  - apple\n  - banana\n  - cherry\n");
    CHECK(!ad.is_err, "yaml_dec with sequence");
    YamlArrayResult ar = yaml_arr(ad.ok, "items");
    CHECK(!ar.is_err && ar.ok.len == 3, "yaml_arr count=3");
    CHECK(!ar.is_err && strcmp(ar.ok.data[0].raw, "apple") == 0, "yaml_arr[0]=apple");
    CHECK(!ar.is_err && strcmp(ar.ok.data[2].raw, "cherry") == 0, "yaml_arr[2]=cherry");

    /* --- yaml_from_json --- */
    const char *json = "{\"name\":\"Alice\",\"age\":30}";
    const char *yaml_out = yaml_from_json(json);
    CHECK(yaml_out && strstr(yaml_out, "name:"), "yaml_from_json has name key");
    CHECK(yaml_out && strstr(yaml_out, "Alice"), "yaml_from_json has Alice value");
    CHECK(yaml_out && strstr(yaml_out, "age:"), "yaml_from_json has age key");

    /* --- yaml_to_json --- */
    const char *yaml_in = "name: Alice\nage: 30\n";
    const char *json_out = yaml_to_json(yaml_in);
    CHECK(json_out && json_out[0] == '{', "yaml_to_json starts with {");
    CHECK(json_out && strstr(json_out, "\"name\""), "yaml_to_json has name key");
    CHECK(json_out && strstr(json_out, "\"Alice\""), "yaml_to_json has Alice value");
    CHECK(json_out && strstr(json_out, "30"), "yaml_to_json has age value");

    /* --- yaml_enc quoting --- */
    const char *plain = yaml_enc("hello");
    CHECK(plain && strcmp(plain, "hello") == 0, "yaml_enc plain passthrough");
    const char *quoted = yaml_enc("key: value");
    CHECK(quoted && quoted[0] == '"', "yaml_enc quotes special chars");

    /* --- quoted strings in YAML --- */
    YamlResult qd = yaml_dec("msg: \"hello world\"\n");
    CHECK(!qd.is_err, "yaml_dec quoted string");
    StrYamlResult qs = yaml_str(qd.ok, "msg");
    CHECK(!qs.is_err && strcmp(qs.ok, "hello world") == 0, "yaml_str strips quotes");

    printf("\n---\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
