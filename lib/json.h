/* SPDX-License-Identifier: MIT */
/* Minimal JSON parser and emitter.
 *
 * Parser: arena-allocated tree, no heap.  Floats stored as fixed-point
 * (scaled by 1e6) to avoid kernel FP.  Nested depth bounded (16 levels).
 * Errors return the byte offset of failure for diagnostics.
 *
 * Emitter: appends JSON text to a byte_buf.
 */

#ifndef MAZU_JSON_H
#define MAZU_JSON_H

#include <mazu/arena.h>
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/stringdef.h>

#define JSON_MAX_DEPTH 16

enum json_type {
    JSON_NULL,
    JSON_BOOL,
    JSON_INT,
    JSON_FLOAT, /* fixed-point: value * 1e6 */
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

struct json_value {
    enum json_type type;
    union {
        bool boolean;
        i64 integer;
        i64 fixed_point; /* scaled by 1e6: 3.14 -> 3140000 */
        struct str string;
        struct {
            struct json_value *items;
            sz count;
        } array;
        struct {
            struct json_kv *pairs;
            sz count;
        } object;
    } v;
};

struct json_kv {
    struct str key;
    struct json_value value;
};

enum {
    JSON_ERR_NONE = 0,
    JSON_ERR_UNEXPECTED_TOKEN,
    JSON_ERR_UNTERMINATED_STRING,
    JSON_ERR_DEPTH_EXCEEDED,
    JSON_ERR_TRAILING_INPUT,
    JSON_ERR_OVERFLOW,
};

struct json_result {
    bool is_error;
    u16 error_code;
    sz error_offset; /* byte position of parse failure */
    struct json_value value;
};

/* Parse JSON text into an arena-allocated tree. */
struct json_result json_parse(struct str input, struct arena *a);

/* JSON emitter that appends to a byte_buf. */
void json_emit_object_start(struct byte_buf *buf);
void json_emit_object_end(struct byte_buf *buf);
void json_emit_array_start(struct byte_buf *buf);
void json_emit_array_end(struct byte_buf *buf);
void json_emit_key(struct byte_buf *buf, struct str key);
void json_emit_string(struct byte_buf *buf, struct str val);
void json_emit_int(struct byte_buf *buf, i64 val);
void json_emit_bool(struct byte_buf *buf, bool val);
void json_emit_null(struct byte_buf *buf);
void json_emit_comma(struct byte_buf *buf);

/* Convenience: emit "key":value pairs (includes comma-readiness). */
void json_emit_key_string(struct byte_buf *buf, struct str key, struct str val);
void json_emit_key_int(struct byte_buf *buf, struct str key, i64 val);
void json_emit_key_bool(struct byte_buf *buf, struct str key, bool val);

/* Lookup a key in a JSON object value.  Returns NULL if not found or if
 * the value is not an object.
 */
struct json_value *json_object_get(struct json_value *obj, struct str key);

#endif /* MAZU_JSON_H */
