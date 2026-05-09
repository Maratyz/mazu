/* SPDX-License-Identifier: MIT */
#include <mazu/selftest.h>

#include <lib/json.h>

/* Stack-allocated arena of 'nbytes' bytes named 'name'. */
#define TEST_ARENA(name, nbytes) \
    byte name##_mem[nbytes];     \
    struct arena name =          \
        arena_new(byte_array_new(name##_mem, sizeof(name##_mem)))

/* Test 1: Parse a JSON object with mixed types and verify structure. */
static i32 test_json_parse_object(void)
{
    TEST_ARENA(a, 4096);
    struct str input =
        STR("{\"key\":\"value\",\"num\":42,\"pi\":3.14,\"arr\":[1,2],\"ok\":"
            "true,\"nil\":null}");

    struct json_result r = json_parse(input, &a);
    if (r.is_error)
        return 1;
    if (r.value.type != JSON_OBJECT)
        return 1;
    if (r.value.v.object.count != 6)
        return 1;

    /* Check string value. */
    struct json_value *v = json_object_get(&r.value, STR("key"));
    if (!v || v->type != JSON_STRING)
        return 1;
    if (!str_is_equal(v->v.string, STR("value")))
        return 1;

    /* Check integer value. */
    v = json_object_get(&r.value, STR("num"));
    if (!v || v->type != JSON_INT || v->v.integer != 42)
        return 1;

    /* Check float as fixed-point (3.14 -> 3140000). */
    v = json_object_get(&r.value, STR("pi"));
    if (!v || v->type != JSON_FLOAT)
        return 1;
    if (v->v.fixed_point != 3140000)
        return 1;

    /* Check array. */
    v = json_object_get(&r.value, STR("arr"));
    if (!v || v->type != JSON_ARRAY || v->v.array.count != 2)
        return 1;
    if (v->v.array.items[0].type != JSON_INT ||
        v->v.array.items[0].v.integer != 1)
        return 1;
    if (v->v.array.items[1].type != JSON_INT ||
        v->v.array.items[1].v.integer != 2)
        return 1;

    /* Check boolean. */
    v = json_object_get(&r.value, STR("ok"));
    if (!v || v->type != JSON_BOOL || !v->v.boolean)
        return 1;

    /* Check null. */
    v = json_object_get(&r.value, STR("nil"));
    if (!v || v->type != JSON_NULL)
        return 1;

    return 0;
}
DEFINE_SELFTEST(json_parse_object, test_json_parse_object);

/* Test 2: Emit JSON and re-parse to verify round-trip. */
static i32 test_json_emit_roundtrip(void)
{
    byte emit_mem[512];
    struct byte_buf buf = byte_buf_new(emit_mem, 0, sizeof(emit_mem));

    json_emit_object_start(&buf);
    json_emit_key_string(&buf, STR("name"), STR("mazu"));
    json_emit_comma(&buf);
    json_emit_key_int(&buf, STR("ver"), 1);
    json_emit_comma(&buf);
    json_emit_key_bool(&buf, STR("ok"), true);
    json_emit_object_end(&buf);

    /* Re-parse the emitted JSON. */
    TEST_ARENA(a, 2048);
    struct str emitted = str_new((char *) buf.dat, buf.len);

    struct json_result r = json_parse(emitted, &a);
    if (r.is_error)
        return 1;
    if (r.value.type != JSON_OBJECT)
        return 1;

    struct json_value *v = json_object_get(&r.value, STR("name"));
    if (!v || v->type != JSON_STRING || !str_is_equal(v->v.string, STR("mazu")))
        return 1;

    v = json_object_get(&r.value, STR("ver"));
    if (!v || v->type != JSON_INT || v->v.integer != 1)
        return 1;

    v = json_object_get(&r.value, STR("ok"));
    if (!v || v->type != JSON_BOOL || !v->v.boolean)
        return 1;

    return 0;
}
DEFINE_SELFTEST(json_emit_roundtrip, test_json_emit_roundtrip);

/* Test 3: error handling. Truncated input returns correct error. */
static i32 test_json_parse_error(void)
{
    TEST_ARENA(a, 1024);

    /* Unterminated string. */
    struct json_result r = json_parse(STR("\"hello"), &a);
    if (!r.is_error || r.error_code != JSON_ERR_UNTERMINATED_STRING)
        return 1;

    /* Trailing input. */
    a = arena_new(byte_array_new(a_mem, sizeof(a_mem)));
    r = json_parse(STR("42 extra"), &a);
    if (!r.is_error || r.error_code != JSON_ERR_TRAILING_INPUT)
        return 1;

    /* Empty input. */
    a = arena_new(byte_array_new(a_mem, sizeof(a_mem)));
    r = json_parse(STR(""), &a);
    if (!r.is_error)
        return 1;

    return 0;
}
DEFINE_SELFTEST(json_parse_error, test_json_parse_error);

/* Test 4: String escape handling. */
static i32 test_json_parse_escapes(void)
{
    TEST_ARENA(a, 2048);

    struct json_result r = json_parse(STR("\"hello\\nworld\\t!\""), &a);
    if (r.is_error)
        return 1;
    if (r.value.type != JSON_STRING)
        return 1;
    /* "hello\nworld\t!" */
    if (r.value.v.string.len != 13)
        return 1;
    if (r.value.v.string.dat[5] != '\n')
        return 1;
    if (r.value.v.string.dat[11] != '\t')
        return 1;

    return 0;
}
DEFINE_SELFTEST(json_parse_escapes, test_json_parse_escapes);

/* Test 5: Negative numbers. */
static i32 test_json_parse_negative(void)
{
    TEST_ARENA(a, 1024);

    struct json_result r = json_parse(STR("-42"), &a);
    if (r.is_error)
        return 1;
    if (r.value.type != JSON_INT || r.value.v.integer != -42)
        return 1;

    a = arena_new(byte_array_new(a_mem, sizeof(a_mem)));
    r = json_parse(STR("-1.5"), &a);
    if (r.is_error)
        return 1;
    if (r.value.type != JSON_FLOAT || r.value.v.fixed_point != -1500000)
        return 1;

    return 0;
}
DEFINE_SELFTEST(json_parse_negative, test_json_parse_negative);
