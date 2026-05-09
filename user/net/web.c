/* SPDX-License-Identifier: MIT */
#include <mazu/arena.h>
#include <mazu/byte.h>
#include <mazu/callout.h>
#include <mazu/error.h>
#include <mazu/fmt.h>
#include <mazu/klog.h>
#include <mazu/kvalloc.h>
#include <mazu/net/arp.h>
#include <mazu/net/tcp.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/ramfs.h>
#include <mazu/sched.h>
#include <mazu/string.h>
#include <mazu/syscall.h>
#include <mazu/time.h>
#include <mazu/vfs.h>

#include "../../lib/json.h"
#include "../shell.h"
#if CONFIG_WEBSOCKET
#include "../../lib/base64.h"
#include "../../lib/sha1.h"
#endif

/* Per-connection resource limits (Task 9y).
 *
 * Limit                     Value   Rejection
 * -------------------------+-------+----------------------------------
 * WEB_MAX_REQUEST_HEADER    8192    431 Request Header Fields Too Large
 * WEB_MAX_REQUEST_PATH      2048    414 URI Too Long
 * WEB_SHELL_POST_BODY_MAX   4096    413 Request Entity Too Large
 * WEB_REQUEST_TIMEOUT_MS    5000    400 Bad Request (header/body timeout)
 * WEB_KEEPALIVE_TIMEOUT_MS  30000   silent close (idle keep-alive)
 * WEB_MAX_CONNS             8       connection rejected (slots full)
 * WEB_MAX_RESPONSE_SIZE     4 MiB   507 Insufficient Storage
 * WS_MAX_FRAME_PAYLOAD      4096    CLOSE 1009 Message Too Big
 *
 * Logging on rejection is currently inconsistent: connection-slot
 * exhaustion uses pr_warn, malformed HTTP uses pr_info, and the body /
 * response-size limits are silent.  Log flooding is bounded by
 * WEB_MAX_CONNS (8 slots) and the TCP layer's per-source-IP connection
 * limit (TCP_MAX_CONNS_PER_IP = 8 in kernel/net/tcp.c).
 */

/* HTTP request parsing and response creation */

enum http_method {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
};

enum http_version {
    HTTP_VERSION_1_0,
    HTTP_VERSION_1_1,
};

enum http_status {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_NOT_MODIFIED = 304,
    HTTP_STATUS_BAD_REQUEST = 400,
    HTTP_STATUS_FORBIDDEN = 403,
    HTTP_STATUS_NOT_FOUND = 404,
    HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
    HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE = 413,
    HTTP_STATUS_URI_TOO_LONG = 414,
    HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
    HTTP_STATUS_INSUFFICIENT_STORAGE = 507,
};

enum http_content_type {
    HTTP_CONTENT_TYPE_TEXT_HTML,
    HTTP_CONTENT_TYPE_TEXT_PLAIN,
    HTTP_CONTENT_TYPE_TEXT_CSS,
    HTTP_CONTENT_TYPE_IMAGE_PNG,
    HTTP_CONTENT_TYPE_IMAGE_JPEG,
    HTTP_CONTENT_TYPE_IMAGE_SVG,
    HTTP_CONTENT_TYPE_APPLICATION_JSON,
    HTTP_CONTENT_TYPE_APPLICATION_JS,
    HTTP_CONTENT_TYPE_TEXT_EVENT_STREAM,
};

struct http_request {
    enum http_method method;
    struct str path;
    enum http_version version;
    bool valid;
};

struct_result(http_method, enum http_method);
struct_result(http_version, enum http_version);

static struct str http_get_file_extension(struct str path)
{
    struct option_sz dot_pos = str_find_char_reverse(path, '.');
    if (dot_pos.is_none)
        return str_new(NULL, 0);

    sz pos = option_sz_checked(dot_pos);
    return str_new(path.dat + pos, path.len - pos);
}

/* Sorted table of (extension, length, content-type) pairs for binary search.
 * Must remain sorted lexicographically by extension for binary search to work.
 */
static const struct {
    const char *ext;
    sz ext_len;
    enum http_content_type type;
} mime_table[] = {
    {".css", 4, HTTP_CONTENT_TYPE_TEXT_CSS},
    {".htm", 4, HTTP_CONTENT_TYPE_TEXT_HTML},
    {".html", 5, HTTP_CONTENT_TYPE_TEXT_HTML},
    {".jpg", 4, HTTP_CONTENT_TYPE_IMAGE_JPEG},
    {".js", 3, HTTP_CONTENT_TYPE_APPLICATION_JS},
    {".json", 5, HTTP_CONTENT_TYPE_APPLICATION_JSON},
    {".png", 4, HTTP_CONTENT_TYPE_IMAGE_PNG},
    {".svg", 4, HTTP_CONTENT_TYPE_IMAGE_SVG},
};

static int mime_cmp(struct str ext, const char *tab, sz tab_len)
{
    sz min_len = ext.len < tab_len ? ext.len : tab_len;
    for (sz i = 0; i < min_len; i++) {
        unsigned char a = (unsigned char) ext.dat[i];
        unsigned char b = (unsigned char) tab[i];
        if (a < b)
            return -1;
        if (a > b)
            return 1;
    }
    if (ext.len < tab_len)
        return -1;
    if (ext.len > tab_len)
        return 1;
    return 0;
}

static enum http_content_type http_get_content_type_from_extension(
    struct str extension)
{
    sz lo = 0, hi = countof(mime_table);
    while (lo < hi) {
        sz mid = lo + (hi - lo) / 2;
        int cmp =
            mime_cmp(extension, mime_table[mid].ext, mime_table[mid].ext_len);
        if (cmp < 0)
            hi = mid;
        else if (cmp > 0)
            lo = mid + 1;
        else
            return mime_table[mid].type;
    }
    return HTTP_CONTENT_TYPE_TEXT_PLAIN;
}

/* URL normalization

 * Maximum request path length (Task 30). Paths longer than this return 414.
 */
#define WEB_MAX_REQUEST_PATH 2048

/* Convert an ASCII hex character to its numeric value (0 to 15).
 * Returns -1 for non-hex characters.
 */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char http_ascii_to_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char) (c + ('a' - 'A'));
    return c;
}

static bool http_str_is_equal_ci(struct str a, struct str b)
{
    if (a.len != b.len)
        return false;
    for (sz i = 0; i < a.len; i++) {
        if (http_ascii_to_lower(a.dat[i]) != http_ascii_to_lower(b.dat[i]))
            return false;
    }
    return true;
}

/* Percent-decode a string in-place into arena-allocated memory.
 * Returns the decoded str. Invalid %XX sequences are passed through.
 */
static struct str percent_decode(struct str src, struct arena *tmp)
{
    char *dec = arena_alloc(tmp, src.len + 1);
    sz dlen = 0;
    for (sz i = 0; i < src.len;) {
        if (src.dat[i] == '%' && i + 2 < src.len) {
            int hi = hex_digit(src.dat[i + 1]);
            int lo = hex_digit(src.dat[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dec[dlen++] = (char) ((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        dec[dlen++] = src.dat[i++];
    }
    return str_new(dec, dlen);
}

/* Look up the value of an HTTP header case-insensitively.
 * Searches only within the header section (before \r\n\r\n).
 * Returns an empty str if the header is not present.
 */
static struct str http_header_value(struct str request_data, struct str name)
{
    struct option_sz sep = str_find_substring(request_data, STR("\r\n\r\n"));
    struct str headers =
        sep.is_none ? request_data
                    : str_new(request_data.dat, option_sz_checked(sep));

    for (sz i = 0; i + name.len + 1 <= headers.len;) {
        bool match = true;
        for (sz k = 0; k < name.len && match; k++) {
            char c = headers.dat[i + k];
            char n = name.dat[k];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (n >= 'a' && n <= 'z')
                n -= 32;
            match = (c == n);
        }
        if (match && headers.dat[i + name.len] == ':') {
            sz val_start = i + name.len + 1;
            while (val_start < headers.len && (headers.dat[val_start] == ' ' ||
                                               headers.dat[val_start] == '\t'))
                val_start++;
            sz val_end = val_start;
            while (val_end < headers.len && headers.dat[val_end] != '\r' &&
                   headers.dat[val_end] != '\n')
                val_end++;
            return str_new(headers.dat + val_start, val_end - val_start);
        }
        /* Skip to the next line. */
        while (i < headers.len && headers.dat[i] != '\n')
            i++;
        i++;
    }
    return str_new(NULL, 0);
}

static bool http_header_has_token(struct str request_data,
                                  struct str name,
                                  struct str token)
{
    struct str value = http_header_value(request_data, name);
    while (value.len > 0) {
        while (value.len > 0 && (value.dat[0] == ' ' || value.dat[0] == '\t' ||
                                 value.dat[0] == ','))
            value = str_new(value.dat + 1, value.len - 1);

        sz i = 0;
        while (i < value.len && value.dat[i] != ',')
            i++;

        struct str part = str_new(value.dat, i);
        while (part.len > 0 && (part.dat[part.len - 1] == ' ' ||
                                part.dat[part.len - 1] == '\t'))
            part.len--;
        if (http_str_is_equal_ci(part, token))
            return true;

        if (i >= value.len)
            break;
        value = str_new(value.dat + i + 1, value.len - i - 1);
    }
    return false;
}

/* Normalize a URI path per RFC 3986 §5.2.4 and the security rules below.
 *  - Rejects incomplete % sequences → returns false.
 *  - Rejects %00 (null byte), %2f/%2F (encoded '/'), %5c/%5C (encoded '\')
 *    → returns false.  These are forbidden regardless of case.
 *  - Decodes all other valid percent-encoded octets.
 *  - Collapses dot segments (. and ..) in-place after decoding.
 *
 * Writes the normalized path into 'out[0..out_cap]' and sets '*out_len'.
 * Returns true on success, false on malformed/dangerous input.
 */
static bool http_normalize_path(struct str path,
                                char *dec,
                                sz dec_cap,
                                sz *out_len)
{
    /* Pass 1: percent-decode into dec[]. */
    sz d = 0;
    for (sz i = 0; i < path.len;) {
        if (d >= dec_cap)
            return false;
        if (path.dat[i] == '%') {
            if (i + 2 >= path.len)
                return false; /* incomplete sequence */
            int hi = hex_digit(path.dat[i + 1]);
            int lo = hex_digit(path.dat[i + 2]);
            if (hi < 0 || lo < 0)
                return false; /* invalid hex digits */
            unsigned ch = (unsigned) (hi << 4) | (unsigned) lo;
            if (ch == 0x00 || ch == 0x2f || ch == 0x5c)
                return false; /* null byte or encoded path separator */
            dec[d++] = (char) ch;
            i += 3;
        } else {
            dec[d++] = path.dat[i++];
        }
    }

    /* Pass 2: dot-segment removal (RFC 3986 §5.2.4) directly on dec[0..d].
     * Uses two indices: r (read) and w (write). w <= r always.
     */
    sz w = 0; /* write cursor */
    sz r = 0; /* read cursor  */
    while (r < d) {
        if (dec[r] != '/') {
            /* Not at a segment boundary: copy verbatim. */
            if (w >= dec_cap)
                return false;
            dec[w++] = dec[r++];
            continue;
        }
        /* At a '/' - find end of the next segment. */
        sz seg_start = r + 1;
        sz seg_end = seg_start;
        while (seg_end < d && dec[seg_end] != '/')
            seg_end++;
        sz seg_len = seg_end - seg_start;

        if (seg_len == 0) {
            /* Double slash: skip the redundant '/'. */
            r++;
            continue;
        }
        if (seg_len == 1 && dec[seg_start] == '.') {
            /* "/.": current-directory reference, skip. */
            r = seg_end;
            continue;
        }
        if (seg_len == 2 && dec[seg_start] == '.' &&
            dec[seg_start + 1] == '.') {
            /* "/..": parent-directory reference - strip last written segment.
             */
            while (w > 0 && dec[w - 1] != '/')
                w--;
            if (w > 0)
                w--; /* also remove the segment's leading '/' */
            r = seg_end;
            continue;
        }
        /* Normal segment: emit '/' + segment text. */
        if (w + 1 + seg_len > dec_cap)
            return false;
        dec[w++] = '/';
        for (sz k = 0; k < seg_len; k++)
            dec[w++] = dec[seg_start + k];
        r = seg_end;
    }

    /* If all segments collapsed (e.g. "/foo/.."), the result is the root "/".
     * Emit it explicitly so the absolute-path check downstream always passes.
     */
    if (w == 0) {
        if (dec_cap < 1)
            return false;
        dec[w++] = '/';
    }

    *out_len = w;
    return true;
}

static struct result_http_method http_parse_method(struct str method_str)
{
    if (str_is_equal(method_str, STR("GET")))
        return result_http_method_ok(HTTP_METHOD_GET);
    if (str_is_equal(method_str, STR("POST")))
        return result_http_method_ok(HTTP_METHOD_POST);
    return result_http_method_error(EINVAL);
}

static struct result_http_version http_parse_version(struct str version_str)
{
    if (str_is_equal(version_str, STR("HTTP/1.1")))
        return result_http_version_ok(HTTP_VERSION_1_1);
    if (str_is_equal(version_str, STR("HTTP/1.0")))
        return result_http_version_ok(HTTP_VERSION_1_0);
    return result_http_version_error(EINVAL);
}

static struct str http_method_to_string(enum http_method method)
{
    switch (method) {
    case HTTP_METHOD_GET:
        return STR("GET");
    case HTTP_METHOD_POST:
        return STR("POST");
    default:
        return STR("Unknown");
    }
}

static struct str http_version_to_string(enum http_version version)
{
    switch (version) {
    case HTTP_VERSION_1_1:
        return STR("HTTP/1.1");
    case HTTP_VERSION_1_0:
        return STR("HTTP/1.0");
    default:
        return STR("Unknown");
    }
}

static struct str http_status_to_string(enum http_status status)
{
    switch (status) {
    case HTTP_STATUS_OK:
        return STR("OK");
    case HTTP_STATUS_NOT_MODIFIED:
        return STR("Not Modified");
    case HTTP_STATUS_BAD_REQUEST:
        return STR("Bad Request");
    case HTTP_STATUS_FORBIDDEN:
        return STR("Forbidden");
    case HTTP_STATUS_NOT_FOUND:
        return STR("Not Found");
    case HTTP_STATUS_METHOD_NOT_ALLOWED:
        return STR("Method Not Allowed");
    case HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE:
        return STR("Request Entity Too Large");
    case HTTP_STATUS_URI_TOO_LONG:
        return STR("URI Too Long");
    case HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE:
        return STR("Request Header Fields Too Large");
    case HTTP_STATUS_INSUFFICIENT_STORAGE:
        return STR("Insufficient Storage");
    default:
        return STR("Unknown");
    }
}

static struct str http_content_type_to_string(
    enum http_content_type content_type)
{
    switch (content_type) {
    case HTTP_CONTENT_TYPE_TEXT_HTML:
        return STR("text/html");
    case HTTP_CONTENT_TYPE_TEXT_PLAIN:
        return STR("text/plain");
    case HTTP_CONTENT_TYPE_TEXT_CSS:
        return STR("text/css");
    case HTTP_CONTENT_TYPE_IMAGE_PNG:
        return STR("image/png");
    case HTTP_CONTENT_TYPE_IMAGE_JPEG:
        return STR("image/jpeg");
    case HTTP_CONTENT_TYPE_IMAGE_SVG:
        return STR("image/svg+xml");
    case HTTP_CONTENT_TYPE_APPLICATION_JSON:
        return STR("application/json");
    case HTTP_CONTENT_TYPE_APPLICATION_JS:
        return STR("application/javascript");
    case HTTP_CONTENT_TYPE_TEXT_EVENT_STREAM:
        return STR("text/event-stream");
    default:
        crash("Invalid content type");
    }
}

static bool is_printable_ascii(char c)
{
    return (c >= 0x20 && c <= 0x7E) || c == 0x09 || c == 0x0A || c == 0x0D;
}

static struct str http_request_header_str(struct str request_data,
                                          struct arena tmp)
{
    struct option_sz end_idx_opt = str_find_substring(request_data, STR("\r\n\r"
                                                                        "\n"));
    if (end_idx_opt.is_none)
        return STR("<Not an HTTP header>");

    sz len = MIN(option_sz_checked(end_idx_opt), 300);
    struct str_buf buf = str_buf_from_arena(&tmp, len);

    for (sz i = 0; i < len; i++) {
        if (is_printable_ascii(request_data.dat[i]))
            str_buf_append_char(&buf, request_data.dat[i]);
        else
            str_buf_append_char(&buf, '?');
    }

    return str_from_buf(buf);
}

static struct http_request http_parse_request(struct str request_data)
{
    struct http_request req = {0};

    struct option_sz first_space = str_find_char(request_data, ' ');
    if (first_space.is_none) {
        req.valid = false;
        return req;
    }

    sz space1_pos = option_sz_checked(first_space);
    struct str method_str = str_new(request_data.dat, space1_pos);
    struct result_http_method method_result = http_parse_method(method_str);
    if (method_result.is_error) {
        req.valid = false;
        return req;
    }
    req.method = result_http_method_checked(method_result);

    struct str remaining = str_new(request_data.dat + space1_pos + 1,
                                   request_data.len - space1_pos - 1);

    struct option_sz second_space = str_find_char(remaining, ' ');
    if (second_space.is_none) {
        req.valid = false;
        return req;
    }

    sz space2_pos = option_sz_checked(second_space);
    req.path = str_new(remaining.dat, space2_pos);

    struct str version_and_rest =
        str_new(remaining.dat + space2_pos + 1, remaining.len - space2_pos - 1);

    struct option_sz newline_pos = str_find_char(version_and_rest, '\r');
    if (newline_pos.is_none) {
        newline_pos = str_find_char(version_and_rest, '\n');
        if (newline_pos.is_none) {
            req.valid = false;
            return req;
        }
    }

    sz nl_pos = option_sz_checked(newline_pos);
    struct str version_str = str_new(version_and_rest.dat, nl_pos);
    struct result_http_version version_result = http_parse_version(version_str);
    if (version_result.is_error) {
        req.valid = false;
        return req;
    }
    req.version = result_http_version_checked(version_result);

    req.valid = true;
    return req;
}

static struct result http_build_header(enum http_status status,
                                       enum http_content_type content_type,
                                       sz body_len,
                                       bool keep_alive,
                                       struct byte_buf *response_buf)
{
    assert(response_buf);

    /* Convert the byte buffer to a string buffer to format the header. Changes
     * to the state of the string buffer do not affect 'response_buf'. Before
     * returning from this function on successful completion, update
     * 'response_buf'. As a side effect, 'response_buf' is not updated if the
     * header was only built partially. This is neat.
     */
    struct str_buf buf = str_buf_from_byte_buf(*response_buf);

    struct result res = fmt(&buf, STR("HTTP/1.1 %u %s\r\n"), (u32) status,
                            http_status_to_string(status));
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, keep_alive ? STR("Connection: keep-alive\r\n")
                                          : STR("Connection: close\r\n"));
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, STR("Server: Mazu\r\n"));
    if (res.is_error)
        return res;

    res = fmt(&buf, STR("Content-Type: %s\r\n"),
              http_content_type_to_string(content_type));
    if (res.is_error)
        return res;

    res = fmt(&buf, STR("Content-Length: %lu\r\n"), (u64) body_len);
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, STR("\r\n"));
    if (res.is_error)
        return res;

    *response_buf = byte_buf_from_str_buf(buf);

    return result_ok();
}

/* Observability counters (item 22). */
static u64 global_web_stats_4xx;
static u64 global_web_stats_5xx;

static struct result http_build_response(enum http_status status,
                                         enum http_content_type content_type,
                                         struct byte_view body,
                                         bool keep_alive,
                                         struct byte_buf *response_buf)
{
    assert(response_buf);

    /* Track 4xx and 5xx responses for the observability endpoint (item 22). */
    if (status >= 400 && status < 500)
        __atomic_add_fetch(&global_web_stats_4xx, 1, __ATOMIC_RELAXED);
    else if (status >= 500)
        __atomic_add_fetch(&global_web_stats_5xx, 1, __ATOMIC_RELAXED);

    /* If the buffer cannot fit the body alone (without a header), do not even
     * bother building the header.
     */
    if (response_buf->cap < response_buf->len + body.len)
        return result_error(ENOMEM);

    struct result res = http_build_header(status, content_type, body.len,
                                          keep_alive, response_buf);
    if (res.is_error)
        return res;

    /* Inject Cache-Control: no-store for all non-static responses.
     * Retract the blank line that terminates the header, append the
     * directive, then re-add the blank line before the body.
     */
    assert(response_buf->len >= 2);
    response_buf->len -= 2; /* remove trailing \r\n */
    {
        struct str_buf hdr = str_buf_from_byte_buf(*response_buf);
        res = str_buf_append(&hdr, STR("Cache-Control: no-store\r\n\r\n"));
        *response_buf = byte_buf_from_str_buf(hdr);
        if (res.is_error)
            return res;
    }

    sz n_appended = byte_buf_append(response_buf, body);
    if (n_appended != body.len)
        return result_error(ENOMEM);

    pr_info(STR("Responding with: %s %s\n"), http_status_to_string(status),
            http_content_type_to_string(content_type));

    return result_ok();
}

/* Forward declaration, defined below after WebSocket section. */
static struct result web_respond(struct tcp_conn *conn,
                                 struct byte_view response,
                                 struct arena tmp);

/* Chunked transfer encoding helpers.
 *
 * http_build_chunked_header() sends response headers with
 * Transfer-Encoding: chunked (no Content-Length).
 * web_send_chunk() sends a single chunk: hex-length CRLF data CRLF.
 * web_end_chunked() sends the terminating 0-length chunk.
 */
static struct result http_build_chunked_header(enum http_status status,
                                               enum http_content_type ctype,
                                               struct byte_buf *response_buf)
{
    assert(response_buf);

    struct str_buf buf = str_buf_from_byte_buf(*response_buf);

    struct result res = fmt(&buf, STR("HTTP/1.1 %u %s\r\n"), (u32) status,
                            http_status_to_string(status));
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, STR("Connection: close\r\n"));
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, STR("Server: Mazu\r\n"));
    if (res.is_error)
        return res;

    res = fmt(&buf, STR("Content-Type: %s\r\n"),
              http_content_type_to_string(ctype));
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, STR("Transfer-Encoding: chunked\r\n"));
    if (res.is_error)
        return res;

    res = str_buf_append(&buf, STR("Cache-Control: no-store\r\n\r\n"));
    if (res.is_error)
        return res;

    *response_buf = byte_buf_from_str_buf(buf);
    return result_ok();
}

static struct result web_send_chunk(struct tcp_conn *conn,
                                    struct byte_view data,
                                    struct arena tmp)
{
    if (data.len == 0)
        return result_ok();

    /* Build "hex-length\r\n" prefix. */
    byte prefix_mem[16];
    struct byte_buf prefix = byte_buf_new(prefix_mem, 0, sizeof(prefix_mem));
    struct str_buf sb = str_buf_from_byte_buf(prefix);
    struct result res = fmt(&sb, STR("%lx\r\n"), (u64) data.len);
    if (res.is_error)
        return res;
    prefix = byte_buf_from_str_buf(sb);

    /* Send prefix. */
    res =
        web_respond(conn, byte_view_new((char *) prefix.dat, prefix.len), tmp);
    if (res.is_error)
        return res;

    /* Send data. */
    res = web_respond(conn, data, tmp);
    if (res.is_error)
        return res;

    /* Send trailing CRLF. */
    return web_respond(conn, byte_view_from_str(STR("\r\n")), tmp);
}

static struct result web_end_chunked(struct tcp_conn *conn, struct arena tmp)
{
    return web_respond(conn, byte_view_from_str(STR("0\r\n\r\n")), tmp);
}

/* Format a u32 ETag as a quoted 8-hex-digit string into buf[10].
 * Returns a str spanning the 10 bytes.
 */
static struct str etag_format(u32 etag, char buf[10])
{
    static const char hex_chars[] = "0123456789abcdef";
    buf[0] = '"';
    for (int i = 0; i < 8; i++)
        buf[1 + i] = hex_chars[(etag >> (28 - i * 4)) & 0xfu];
    buf[9] = '"';
    return str_new(buf, 10);
}

/* Build a minimal HTTP 304 Not Modified response (no body) including the
 * ETag header so clients can update their cached copy's validator.
 * Used by http_serve_file() for conditional GET (Task 32).
 */
static struct result http_build_304(u32 etag,
                                    bool keep_alive,
                                    struct byte_buf *response_buf)
{
    assert(response_buf);

    struct str_buf buf = str_buf_from_byte_buf(*response_buf);

    struct result res = str_buf_append(&buf, STR("HTTP/1.1 304 Not "
                                                 "Modified\r\n"));
    if (res.is_error)
        return res;
    res = str_buf_append(&buf, keep_alive ? STR("Connection: keep-alive\r\n")
                                          : STR("Connection: close\r\n"));
    if (res.is_error)
        return res;
    char etag_buf[10];
    res = fmt(&buf, STR("ETag: %s\r\n"), etag_format(etag, etag_buf));
    if (res.is_error)
        return res;
    res = str_buf_append(&buf, STR("Cache-Control: no-store\r\n"));
    if (res.is_error)
        return res;
    res = str_buf_append(&buf, STR("\r\n"));
    if (res.is_error)
        return res;

    *response_buf = byte_buf_from_str_buf(buf);
    pr_info(STR("Responding with: 304 Not Modified\n"));
    return result_ok();
}

#define HTML_PAGE(title, content)                                             \
    "<!DOCTYPE html>"                                                         \
    "<html lang=\"en\"><head>"                                                \
    "<meta charset=\"UTF-8\">"                                                \
    "<meta name=\"viewport\" content=\"width=device-width, "                  \
    "initial-scale=1.0\">"                                                    \
    "<title>" title                                                           \
    "</title>"                                                                \
    "</head><body>" content "<footer><hr/><small>Served by Mazu (" GIT_COMMIT \
    ")</small></footer>"                                                      \
    "</body></html>"

static struct str forbidden_body =
    STR_STATIC(HTML_PAGE("403 Forbidden",
                         "<h1>403 Forbidden</h1><p>Directory "
                         "listing not allowed.</p>"));
static struct str not_found_body =
    STR_STATIC(HTML_PAGE("404 Not Found",
                         "<h1>404 Not Found</h1><p>The "
                         "requested file was not found.</p>"));
static struct str bad_request_body =
    STR_STATIC(HTML_PAGE("400 Bad Request",
                         "<h1>400 Bad "
                         "Request</h1><p>Invalid HTTP "
                         "request.</p>"));
static struct str request_too_large_body =
    STR_STATIC(HTML_PAGE("431 Request Header Fields Too Large",
                         "<h1>431 Request Header "
                         "Fields Too "
                         "Large</h1><p>Request "
                         "header too large.</p>"));
static struct str post_body_too_large_body =
    STR_STATIC(HTML_PAGE("413 Request Entity Too Large",
                         "<h1>413 Request "
                         "Entity Too "
                         "Large</h1><p>POST "
                         "body exceeds size "
                         "limit.</p>"));
static struct str insufficient_storage_body =
    STR_STATIC(HTML_PAGE("507 Insufficient Storage",
                         "<h1>507 Insufficient "
                         "Storage</h1><p>The "
                         "server does not have "
                         "enough memory to store "
                         "your request.</p>"));
static struct str method_not_allowed_body =
    STR_STATIC(HTML_PAGE("405 Method Not Allowed",
                         "<h1>405 Method Not "
                         "Allowed</h1><p>Only GET is "
                         "supported for static "
                         "files.</p>"));

/* Serve a static file from the RAM filesystem (Task 32: ETag/304 support).
 * 'if_none_match' is the client's If-None-Match header value (may be empty).
 * 'norm_buf' is an arena-allocated scratch buffer of at least
 * WEB_MAX_REQUEST_PATH bytes used for URL normalization (Task 29).
 */
static struct result http_serve_file(struct ram_fs_node *root,
                                     struct str path,
                                     struct str if_none_match,
                                     bool keep_alive,
                                     char *norm_buf,
                                     sz norm_cap,
                                     struct byte_buf *response_buf,
                                     struct byte_view *out_file_body)
{
    assert(root);
    assert(response_buf);

    if (path.len == 0 || (path.len == 1 && path.dat[0] == '/')) {
        path = STR("/index.html");
    }

    /* Task 29: normalize path (percent-decode + dot-segment collapse + reject
     * forbidden encodings).  The normalized result replaces path.
     */
    sz norm_len = 0;
    if (!http_normalize_path(path, norm_buf, norm_cap, &norm_len)) {
        pr_info(STR("Rejecting malformed or dangerous path \"%s\"\n"), path);
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(bad_request_body), false, response_buf);
    }
    path = str_new(norm_buf, norm_len);

    /* Require an absolute path (ram_fs only accepts these). */
    if (path.len == 0 || path.dat[0] != '/') {
        pr_info(STR("Path \"%s\" doesn't start with a '/' character\n"), path);
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(not_found_body), false, response_buf);
    }

    /* Reject any ".." component that survived normalization
     * (belt-and-suspenders). A correctly normalized path should not contain
     * ".." but the guard is kept to protect against edge cases in the
     * normalizer.
     */
    if (!str_find_substring(path, STR("..")).is_none) {
        pr_info(STR("Rejecting path \"%s\": traversal component after "
                    "normalization\n"),
                path);
        return http_build_response(
            HTTP_STATUS_FORBIDDEN, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(forbidden_body), false, response_buf);
    }

    struct result_ram_fs_node file_result = ram_fs_open(root, path);
    if (file_result.is_error) {
        pr_info(STR("Failed to find file \"%s\"\n"), path);
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(not_found_body), false, response_buf);
    }

    struct ram_fs_node *file_node = result_ram_fs_node_checked(file_result);
    if (file_node->type != RAM_FS_TYPE_FILE) {
        pr_info(STR("Cannot serve request for \"%s\"; it's not a file "
                    "(type=%d)\n"),
                path, file_node->type);
        return http_build_response(
            HTTP_STATUS_FORBIDDEN, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(forbidden_body), false, response_buf);
    }

    char etag_formatted[10];
    struct str etag_str = etag_format(file_node->etag, etag_formatted);

    /* RFC 7232 §3.2: "If-None-Match: *" matches any existing resource. */
    if (if_none_match.len > 0 && (str_is_equal(if_none_match, STR("*")) ||
                                  str_is_equal(if_none_match, etag_str))) {
        pr_info(STR("304 Not Modified for \"%s\" (ETag %s)\n"), path, etag_str);
        return http_build_304(file_node->etag, keep_alive, response_buf);
    }

    struct str extension = http_get_file_extension(path);
    enum http_content_type content_type =
        http_get_content_type_from_extension(extension);
    struct byte_view body = byte_view_from_buf(file_node->data);

    /* Zero-copy optimization (P2.9f): build headers into response_buf
     * but pass file body as a separate byte_view referencing ramfs data
     * directly.  The send state machine transmits headers first, then
     * the file body without an intermediate memcpy.
     */
    struct result res = http_build_header(HTTP_STATUS_OK, content_type,
                                          body.len, keep_alive, response_buf);
    if (res.is_error)
        return res;

    /* Retract the trailing \r\n to append ETag and Cache-Control,
     * then re-close the headers.
     */
    assert(response_buf->len >= 2);
    response_buf->len -= 2;
    struct str_buf etag_sbuf = str_buf_from_byte_buf(*response_buf);
    res = str_buf_append(&etag_sbuf, STR("Cache-Control: no-store\r\n"));
    if (res.is_error)
        return res;
    res = fmt(&etag_sbuf, STR("ETag: %s\r\n\r\n"), etag_str);
    if (res.is_error)
        return res;
    *response_buf = byte_buf_from_str_buf(etag_sbuf);

    /* Pass file body as zero-copy reference (no memcpy). */
    *out_file_body = body;

    pr_info(STR("Serving file \"%s\" (ETag %s)\n"), path, etag_str);
    return result_ok();
}

static struct str stats_format = STR_STATIC(
    "{"
    "\"uptime-ms\": %lu,"
    "\"connections\": %ld,"
    "\"sbq-mem-bytes\": %ld,"
    "\"recv-mem-bytes\": %ld,"
    "\"tx-bytes\": %ld,"
    "\"rx-bytes\": %ld,"
    "\"retransmits\": %ld,"
    "\"conn-pool-exhaustion\": %ld,"
    "\"http-4xx\": %lu,"
    "\"http-5xx\": %lu,"
    "\"commit\": \"" GIT_COMMIT
    "\""
    "}\n");

static struct result http_serve_stats(bool keep_alive,
                                      struct byte_buf *response_buf,
                                      struct arena tmp)
{
    assert(response_buf);

    struct tcp_stats stats = tcp_stats_get();
    struct str_buf sbuf = str_buf_from_arena(&tmp, 700);
    struct result res =
        fmt(&sbuf, stats_format, stats.uptime.ms, stats.n_connections,
            stats.sbq_mem, stats.recv_mem, stats.bytes_tx, stats.bytes_rx,
            stats.retransmits, stats.pool_exhaustion,
            __atomic_load_n(&global_web_stats_4xx, __ATOMIC_RELAXED),
            __atomic_load_n(&global_web_stats_5xx, __ATOMIC_RELAXED));
    if (res.is_error)
        return res;

    return http_build_response(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_APPLICATION_JSON,
        byte_view_from_str(str_from_buf(sbuf)), keep_alive, response_buf);
}

/* Query string and body helpers

 * Split path at '?', returning the path component and writing the query string
 * (everything after '?') to *query_out.  If there is no '?', *query_out is an
 * empty string.
 */
static struct str http_split_path_query(struct str path_with_query,
                                        struct str *query_out)
{
    assert(query_out);
    for (sz i = 0; i < path_with_query.len; i++) {
        if (path_with_query.dat[i] == '?') {
            *query_out = str_new(path_with_query.dat + i + 1,
                                 path_with_query.len - i - 1);
            return str_new(path_with_query.dat, i);
        }
    }
    *query_out = str_new(NULL, 0);
    return path_with_query;
}

static bool http_parse_u64(struct str s, u64 *out)
{
    if (s.len == 0)
        return false;

    u64 v = 0;
    for (sz i = 0; i < s.len; i++) {
        char c = s.dat[i];
        if (c < '0' || c > '9')
            return false;
        u64 digit = (u64) (c - '0');
        if (MUL_OVERFLOW(v, (u64) 10) || ADD_OVERFLOW(v * 10, digit))
            return false;
        v = v * 10 + digit;
    }
    *out = v;
    return true;
}

static bool http_parse_u32(struct str s, u32 *out)
{
    u64 value = 0;
    if (!http_parse_u64(s, &value) || value > U32_MAX)
        return false;
    *out = (u32) value;
    return true;
}

/* Find the value of 'key' in a query string like "sid=1&from=42". Returns empty
 * string if the key is not present.
 */
static struct str http_query_get_param(struct str query, struct str key)
{
    sz i = 0;
    while (i < query.len) {
        /* Find the '=' for this token. */
        sz eq = i;
        while (eq < query.len && query.dat[eq] != '=' && query.dat[eq] != '&')
            eq++;
        if (eq >= query.len || query.dat[eq] != '=') {
            /* Valueless parameter (e.g. "flag&sid=1") - skip it and continue.
             */
            i = (eq < query.len) ? eq + 1 : query.len;
            continue;
        }

        struct str k = str_new(query.dat + i, eq - i);
        sz val_start = eq + 1;
        sz val_end = val_start;
        while (val_end < query.len && query.dat[val_end] != '&')
            val_end++;

        if (str_is_equal(k, key))
            return str_new(query.dat + val_start, val_end - val_start);

        i = val_end;
        if (i < query.len && query.dat[i] == '&')
            i++;
    }
    return str_new(NULL, 0);
}

/* Return the value of "Content-Length:" from the HTTP headers, or 0.
 * Uses http_header_value which already does case-insensitive search
 * clamped to the header section (before \r\n\r\n).
 */
static bool http_parse_content_length(struct str request_data, sz *out_len)
{
    struct str val = http_header_value(request_data, STR("Content-Length"));
    if (val.len == 0) {
        *out_len = 0;
        return true;
    }

    u64 parsed = 0;
    if (!http_parse_u64(val, &parsed) || parsed > (u64) ((sz) -1))
        return false;

    *out_len = (sz) parsed;
    return true;
}

/* Return the body of the HTTP request (bytes after \r\n\r\n). */
static struct str http_get_body(struct str request_data)
{
    struct option_sz pos = str_find_substring(request_data, STR("\r\n\r\n"));
    if (pos.is_none)
        return str_new(NULL, 0);
    sz body_start = option_sz_checked(pos) + 4;
    return str_new(request_data.dat + body_start,
                   request_data.len - body_start);
}

/* Shell API handlers */

static struct str shell_bad_sid_body = STR_STATIC(
    "400 Bad Request: missing or "
    "invalid sid\n");
static struct str shell_bad_tok_body = STR_STATIC(
    "400 Bad Request: missing "
    "tok "
    "parameter\n");
static struct str shell_bad_from_body =
    STR_STATIC("400 Bad Request: invalid from parameter\n");
static struct str shell_not_found_body = STR_STATIC(
    "404 Not Found: session "
    "does not exist\n");
static struct str shell_forbidden_body = STR_STATIC(
    "403 Forbidden: invalid "
    "session token\n");

static void shell_append_output_since(struct shell_session *sess,
                                      u64 from,
                                      struct str_buf *sbuf)
{
    assert(sess);
    assert(sbuf);

    if (from > sess->write_total)
        from = sess->write_total;

    if (sess->write_total - from > SHELL_OUT_BUF_SIZE) {
        str_buf_append(sbuf, STR("[data lost]\n"));
        from = sess->write_total - SHELL_OUT_BUF_SIZE;
    }

    u64 avail = sess->write_total - from;
    for (u64 i = 0; i < avail; i++) {
        sz pos = (sz) ((from + i) % SHELL_OUT_BUF_SIZE);
        str_buf_append_char(sbuf, sess->out[pos]);
    }
}

/* GET  /api/shell/in?sid=<id>             - create session, returns
 * "tok=<token>\n" POST /api/shell/in?sid=<id>&tok=<token>   body = command text
 */
static struct result http_serve_shell_in(struct str query,
                                         struct str body,
                                         bool keep_alive,
                                         struct byte_buf *response_buf,
                                         struct arena tmp)
{
    struct str sid_str = http_query_get_param(query, STR("sid"));
    if (sid_str.len == 0)
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_sid_body), false, response_buf);

    u32 sid = 0;
    if (!http_parse_u32(sid_str, &sid))
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_sid_body), false, response_buf);
    struct str tok_str = http_query_get_param(query, STR("tok"));

    if (tok_str.len == 0) {
        /* No token: create a new session and return the token to the client. */
        struct shell_session *sess = shell_create(sid);
        struct str_buf tbuf = str_buf_from_arena(&tmp, 32);
        fmt(&tbuf, STR("tok=%lu\n"), sess->token);
        return http_build_response(HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_PLAIN,
                                   byte_view_from_str(str_from_buf(tbuf)),
                                   keep_alive, response_buf);
    }

    u64 tok = 0;
    if (!http_parse_u64(tok_str, &tok))
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_tok_body), false, response_buf);
    struct shell_session *sess = shell_find(sid);
    if (!sess)
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_not_found_body), false, response_buf);

    if (sess->token != tok)
        return http_build_response(
            HTTP_STATUS_FORBIDDEN, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_forbidden_body), false, response_buf);

    pr_info(STR("shell_in: sid=%lu tok=%lu body.len=%ld body=\"%s\"\n"),
            (u64) sid, tok, body.len, body);

    u64 before = sess->write_total;
    shell_exec(sess, body, tmp);

    /* Return the command output directly so the client does not need to poll.
     * This eliminates the race between POST (execute) and GET (read output).
     */
    struct str_buf sbuf = str_buf_from_arena(&tmp, SHELL_OUT_BUF_SIZE + 64);
    shell_append_output_since(sess, before, &sbuf);

    pr_info(STR("shell_in: before=%lu after=%lu sbuf.len=%ld\n"), before,
            sess->write_total, sbuf.len);

    return http_build_response(HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_PLAIN,
                               byte_view_from_str(str_from_buf(sbuf)),
                               keep_alive, response_buf);
}

/* GET /api/shell/out?sid=<id>&tok=<token>&from=<offset> */
static struct result http_serve_shell_out(struct str query,
                                          bool keep_alive,
                                          struct byte_buf *response_buf,
                                          struct arena tmp)
{
    struct str sid_str = http_query_get_param(query, STR("sid"));
    struct str tok_str = http_query_get_param(query, STR("tok"));
    struct str from_str = http_query_get_param(query, STR("from"));

    if (sid_str.len == 0)
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_sid_body), false, response_buf);

    if (tok_str.len == 0)
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_tok_body), false, response_buf);

    u32 sid = 0;
    u64 tok = 0;
    if (!http_parse_u32(sid_str, &sid))
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_sid_body), false, response_buf);
    if (!http_parse_u64(tok_str, &tok))
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_tok_body), false, response_buf);

    struct shell_session *sess = shell_find(sid);
    if (!sess)
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_not_found_body), false, response_buf);

    if (sess->token != tok)
        return http_build_response(
            HTTP_STATUS_FORBIDDEN, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_forbidden_body), false, response_buf);

    u64 from = 0;
    if (from_str.len > 0 && !http_parse_u64(from_str, &from))
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_PLAIN,
            byte_view_from_str(shell_bad_from_body), false, response_buf);

    /* Build response body: bytes from 'from' to write_total. Use the
     * response_buf directly; the header will be prepended last. Determine body
     * length first; build into a temporary str_buf.
     */
    struct str_buf sbuf = str_buf_from_arena(&tmp, SHELL_OUT_BUF_SIZE + 64);

    shell_append_output_since(sess, from, &sbuf);

    return http_build_response(HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_PLAIN,
                               byte_view_from_str(str_from_buf(sbuf)),
                               keep_alive, response_buf);
}

/* Determine keep-alive intent from the HTTP headers only. HTTP/1.1: keep-alive
 * by default unless "Connection: close" is present. HTTP/1.0: close by default.
 */
static bool http_request_keep_alive(struct str request_data,
                                    enum http_version version)
{
    if (version != HTTP_VERSION_1_1)
        return false;

    if (http_header_value(request_data, STR("Connection")).len == 0)
        return true;

    return !http_header_has_token(request_data, STR("Connection"),
                                  STR("close"));
}

/* /debug endpoint (Task 34) Guarded by CONFIG_DEBUG_ENDPOINT; never compiled in
 * release builds.
 */

#if CONFIG_DEBUG_ENDPOINT

struct debug_task_ctx {
    struct str_buf *sbuf;
};

static void debug_task_cb(struct sched_task_info info, void *ctx_ptr)
{
    struct debug_task_ctx *ctx = ctx_ptr;
    fmt(ctx->sbuf, STR("  id=%-3hu  %-8s  cpu=%-12lu us  cb=%lx\n"),
        (u32) info.id, td_state_name(info.state), info.cpu_time_us,
        (u64) (uptr) info.callback);
}

/* Serve a live kernel introspection page (Task 34).
 * Reports: task list, TCP pool stats, kvalloc free pages.
 * No heap allocations beyond the arena.
 */
static struct result http_serve_debug(bool keep_alive,
                                      struct byte_buf *response_buf,
                                      struct arena tmp)
{
    struct str_buf sbuf = str_buf_from_arena(&tmp, 2048);
    struct debug_task_ctx task_ctx = {.sbuf = &sbuf};

    str_buf_append(&sbuf, STR("=== Mazu Debug ===\n\nTasks:\n"));
    sched_for_each_task(debug_task_cb, &task_ctx);

    struct tcp_stats ts = tcp_stats_get();
    fmt(&sbuf,
        STR("\nTCP:\n"
            "  connections:      %ld\n"
            "  pool_exhaustion:  %ld\n"
            "  retransmits:      %ld\n"
            "  bytes_tx:         %ld\n"
            "  bytes_rx:         %ld\n"),
        ts.n_connections, ts.pool_exhaustion, ts.retransmits, ts.bytes_tx,
        ts.bytes_rx);

    struct kvalloc_stats ks = kvalloc_stats_get();
    fmt(&sbuf, STR("\nKVAlloc:\n  free_pages:  %ld\n"), ks.free_pages);

    return http_build_response(HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_PLAIN,
                               byte_view_from_str(str_from_buf(sbuf)),
                               keep_alive, response_buf);
}

#endif /* CONFIG_DEBUG_ENDPOINT */

/* JSON API endpoints.
 *
 * Shared context for JSON array iteration callbacks.
 */
struct api_iter_ctx {
    struct str_buf *sbuf;
    struct arena *tmp;
    sz count;
};

/* Emit a comma separator before the second and subsequent array elements. */
static void api_iter_comma(struct api_iter_ctx *ctx)
{
    if (ctx->count > 0)
        str_buf_append_char(ctx->sbuf, ',');
    ctx->count++;
}

/* /api/stats: extended kernel stats as JSON. */

static void api_stats_task_cb(struct sched_task_info info, void *ctx_ptr)
{
    struct api_iter_ctx *ctx = ctx_ptr;
    api_iter_comma(ctx);
    fmt(ctx->sbuf,
        STR("{\"id\":%hu,\"state\":\"%s\",\"prio\":%hhu,\"cpu_us\":%lu,"
            "\"last_activity_ms\":%lu,\"hung\":%s}"),
        (u32) info.id, td_state_name(info.state), (u32) info.prio,
        info.cpu_time_us, info.last_activity_ms,
        info.hung ? STR("true") : STR("false"));
}

static struct result http_serve_api_stats(bool keep_alive,
                                          struct byte_buf *response_buf,
                                          struct arena tmp)
{
    assert(response_buf);

    struct tcp_stats ts = tcp_stats_get();
    struct kvalloc_stats ks = kvalloc_stats_get();
    struct sched_latency_stats sls;
    sched_get_latency_stats(&sls);
    struct sched_watchdog_stats wds;
    sched_get_watchdog_stats(&wds);
    struct callout_stats cos;
    callout_get_stats(&cos);
    struct sched_ctxsw_stats csw;
    sched_get_ctxsw_stats(&csw);
    struct str_buf sbuf = str_buf_from_arena(&tmp, 4096);

    fmt(&sbuf,
        STR("{\"uptime_ms\":%lu,\"memory\":{\"total_pages\":%ld,\"free_pages\":"
            "%ld},"),
        ts.uptime.ms, ks.total_pages, ks.free_pages);
    fmt(&sbuf,
        STR("\"tcp\":{\"connections\":%ld,\"bytes_tx\":%ld,\"bytes_rx\":%ld,"
            "\"retransmits\":%ld},"),
        ts.n_connections, ts.bytes_tx, ts.bytes_rx, ts.retransmits);
    fmt(&sbuf, STR("\"http\":{\"4xx\":%lu,\"5xx\":%lu},"),
        __atomic_load_n(&global_web_stats_4xx, __ATOMIC_RELAXED),
        __atomic_load_n(&global_web_stats_5xx, __ATOMIC_RELAXED));
    fmt(&sbuf,
        STR("\"scheduler\":{\"wakeup_latency_max_us\":%lu,\"wakeup_latency_"
            "hist\":[%lu,%lu,%lu,%lu,%lu,%lu],"
            "\"nr_ctxsw\":%lu,\"ctxsw_avg_cycles\":%lu,"
            "\"ctxsw_max_cycles\":%lu,\"nr_migrations\":%lu,"
            "\"nr_remote_wakeups\":%lu},"),
        sls.wakeup_latency_max_us, sls.wakeup_latency_hist[0],
        sls.wakeup_latency_hist[1], sls.wakeup_latency_hist[2],
        sls.wakeup_latency_hist[3], sls.wakeup_latency_hist[4],
        sls.wakeup_latency_hist[5], csw.nr_ctxsw, csw.avg_cycles,
        csw.max_cycles, csw.nr_migrations, csw.nr_remote_wakeups);

    /* Per-CPU interrupt and scheduler counters. */
    str_buf_append(&sbuf, STR("\"cpus\":["));
    u32 ncpus = nr_cpus_online;
    for (u32 i = 0; i < ncpus; i++) {
        struct pcpu_irq_stats is = pcpu_irq_stats_get(i);
        struct pcpu_sched_stats ss = pcpu_sched_stats_get(i);
        struct pcpu_ctxsw_stats cs = pcpu_ctxsw_stats_get(i);
        if (i > 0)
            str_buf_append_char(&sbuf, ',');
        struct sched_hart_watchdog_stats hwd;
        sched_get_hart_watchdog(i, &hwd);
        fmt(&sbuf,
            STR("{\"cpu\":%u,\"nr_timer\":%lu,\"nr_exti\":%lu,\"nr_ssi\":%lu,"
                "\"nr_enqueue\":%lu,\"nr_dequeue\":%lu,"
                "\"nr_sched_ops\":%u,\"max_sched_ops\":%u,"
                "\"total_wait_ticks\":%lu,"
                "\"nr_ctxsw\":%lu,\"ctxsw_cycles_total\":%lu,"
                "\"ctxsw_cycles_max\":%lu,\"nr_migrations\":%lu,"
                "\"nr_remote_wakeups\":%lu,"
                "\"hart_load\":%lu,"
                "\"heartbeat_age_us\":%lu,\"heartbeat_stale\":%s,"
                "\"heartbeat_idle\":%s}"),
            i, is.nr_timer, is.nr_exti, is.nr_ssi, ss.nr_enqueue, ss.nr_dequeue,
            (u32) ss.nr_sched_ops, (u32) ss.max_sched_ops, ss.total_wait_ticks,
            cs.nr_ctxsw, cs.ctxsw_cycles_total, cs.ctxsw_cycles_max,
            cs.nr_migrations, cs.nr_remote_wakeups, pcpu_array[i].hart_load,
            hwd.heartbeat_age_us, hwd.stale ? STR("true") : STR("false"),
            hwd.idle ? STR("true") : STR("false"));
    }
    str_buf_append(&sbuf, STR("],"));

    fmt(&sbuf,
        STR("\"callout\":{\"dispatched\":%lu,\"missed\":%lu,"
            "\"max_late_us\":%lu,\"timer_writes\":%lu,"
            "\"timer_skips\":%lu,"
            "\"late_hist\":[%lu,%lu,%lu,%lu,%lu,%lu]},"),
        cos.dispatched, cos.missed, cos.max_late_us, cos.timer_writes,
        cos.timer_skips, cos.late_hist[0], cos.late_hist[1], cos.late_hist[2],
        cos.late_hist[3], cos.late_hist[4], cos.late_hist[5]);

    struct syscall_security_stats sss = syscall_security_stats_get();
    fmt(&sbuf, STR("\"security\":{\"nr_denied\":%lu,\"nr_enosys\":%lu},"),
        sss.nr_denied, sss.nr_enosys);

    fmt(&sbuf, STR("\"watchdog\":{\"nr_hung\":%u,\"nr_warnings\":%u},"),
        wds.nr_hung, wds.nr_warnings);

    /* Scheduling domain stats (named kernel domains). */
    str_buf_append(&sbuf, STR("\"domains\":["));
    for (u32 d = 0; d < SCHED_DOMAIN_COUNT; d++) {
        struct sched_domain_stats dstat;
        if (sched_domain_get_stats(d, &dstat) == 0) {
            if (d > 0)
                str_buf_append_char(&sbuf, ',');
            fmt(&sbuf,
                STR("{\"id\":%u,\"quantum_ticks\":%lu,\"period_ticks\":%lu,"
                    "\"consumed_ticks\":%lu,\"nr_members\":%u,"
                    "\"state\":\"%s\"}"),
                d, dstat.quantum_ticks, dstat.period_ticks,
                dstat.consumed_ticks, dstat.nr_members,
                dstat.state == 0 ? STR("active") : STR("depleted"));
        }
    }
    str_buf_append(&sbuf, STR("],"));

    str_buf_append(&sbuf, STR("\"tasks\":["));

    struct api_iter_ctx task_ctx = {.sbuf = &sbuf, .tmp = &tmp, .count = 0};
    sched_for_each_task(api_stats_task_cb, &task_ctx);

    str_buf_append(&sbuf, STR("]}"));

    return http_build_response(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_APPLICATION_JSON,
        byte_view_from_str(str_from_buf(sbuf)), keep_alive, response_buf);
}

/* /api/tcp - TCP connection table as JSON. */

static void api_tcp_conn_cb(struct tcp_conn_info info, void *ctx_ptr)
{
    struct api_iter_ctx *ctx = ctx_ptr;
    api_iter_comma(ctx);
    fmt(ctx->sbuf,
        STR("{\"host\":\"%s:%hu\",\"peer\":\"%s:%hu\",\"state\":\"%s\","
            "\"cwnd\":%u,\"ssthresh\":%u,"
            "\"pkts_sent\":%lu,\"pkts_recv\":%lu,"
            "\"bytes_sent\":%lu,\"bytes_recv\":%lu,"
            "\"retransmits\":%u,\"dup_acks\":%u,\"ooo_segments\":%u}"),
        ipv4_addr_format(info.host_addr, ctx->tmp), (u32) info.host_port,
        ipv4_addr_format(info.peer_addr, ctx->tmp), (u32) info.peer_port,
        info.state_name, info.cwnd, info.ssthresh, info.pkts_sent,
        info.pkts_recv, info.bytes_sent, info.bytes_recv, info.retransmits,
        info.dup_acks, info.ooo_segments);
}

static struct result http_serve_api_tcp(bool keep_alive,
                                        struct byte_buf *response_buf,
                                        struct arena tmp)
{
    assert(response_buf);

    struct str_buf sbuf = str_buf_from_arena(&tmp, 2048);
    str_buf_append(&sbuf, STR("{\"connections\":["));

    struct api_iter_ctx ctx = {.sbuf = &sbuf, .tmp = &tmp, .count = 0};
    tcp_for_each_conn(api_tcp_conn_cb, &ctx);

    str_buf_append(&sbuf, STR("]}"));

    return http_build_response(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_APPLICATION_JSON,
        byte_view_from_str(str_from_buf(sbuf)), keep_alive, response_buf);
}

/* /api/arp - ARP table as JSON. */

static void api_arp_entry_cb(struct arp_entry_info info, void *ctx_ptr)
{
    struct api_iter_ctx *ctx = ctx_ptr;
    api_iter_comma(ctx);
    fmt(ctx->sbuf, STR("{\"ip\":\"%s\",\"mac\":\"%s\",\"age_ms\":%lu}"),
        ipv4_addr_format(info.ip_addr, ctx->tmp),
        mac_addr_format(info.mac_addr, ctx->tmp), info.age_ms);
}

static struct result http_serve_api_arp(bool keep_alive,
                                        struct byte_buf *response_buf,
                                        struct arena tmp)
{
    assert(response_buf);

    struct str_buf sbuf = str_buf_from_arena(&tmp, 1024);
    str_buf_append(&sbuf, STR("{\"entries\":["));

    struct api_iter_ctx ctx = {.sbuf = &sbuf, .tmp = &tmp, .count = 0};
    arp_for_each(api_arp_entry_cb, &ctx);

    str_buf_append(&sbuf, STR("]}"));

    return http_build_response(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_APPLICATION_JSON,
        byte_view_from_str(str_from_buf(sbuf)), keep_alive, response_buf);
}

#if CONFIG_WEB_TELEMETRY

/* /api/klog: kernel log ring buffer contents as JSON. */

static struct result http_serve_api_klog(bool keep_alive,
                                         struct byte_buf *response_buf,
                                         struct arena tmp)
{
    assert(response_buf);

    /* Peek at the ring buffer (non-destructive read). */
    char *log_data = arena_alloc(&tmp, KLOG_SIZE);
    sz log_len = klog_peek(log_data, KLOG_SIZE);

    /* Build JSON: {"dropped":<N>,"log":"<escaped>"}
     * Worst-case expansion: control chars → \u00XX (6 bytes each),
     * plus quotes, "dropped" prefix, and closing brace.
     */
    struct str_buf sbuf = str_buf_from_arena(&tmp, KLOG_SIZE * 6 + 256);
    fmt(&sbuf, STR("{\"dropped\":%lu,\"log\":"), klog_dropped());

    /* Delegate escaping to json_emit_string (handles \u00XX for control chars).
     */
    struct byte_buf jbuf = byte_buf_from_str_buf(sbuf);
    json_emit_string(&jbuf, str_new(log_data, log_len));
    sbuf = str_buf_from_byte_buf(jbuf);
    str_buf_append_char(&sbuf, '}');

    return http_build_response(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_APPLICATION_JSON,
        byte_view_from_str(str_from_buf(sbuf)), keep_alive, response_buf);
}

#endif /* CONFIG_WEB_TELEMETRY */

/* /api/fs/read?path=X - file content as text/plain.
 * Capped at 256 KiB to avoid arena exhaustion.
 */

#define VFS_READ_MAX ((sz) 256 * 1024)

static struct result http_serve_api_fs_read(struct str query,
                                            bool keep_alive,
                                            struct byte_buf *response_buf,
                                            struct arena tmp)
{
    assert(response_buf);

    struct str path = http_query_get_param(query, STR("path"));
    if (path.len == 0) {
        struct str body = STR("{\"error\":\"missing path\"}");
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), keep_alive, response_buf);
    }

    path = percent_decode(path, &tmp);

    if (path.len == 0 || path.dat[0] != '/') {
        struct str body = STR("{\"error\":\"invalid path\"}");
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), keep_alive, response_buf);
    }

    struct result_vfs_stat st = vfs_stat(path);
    if (st.is_error || result_vfs_stat_checked(st).type != VFS_TYPE_FILE) {
        struct str body = STR("{\"error\":\"not found\"}");
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), keep_alive, response_buf);
    }

    sz file_size = result_vfs_stat_checked(st).size;
    if (file_size > VFS_READ_MAX) {
        struct str body = STR("{\"error\":\"file too large\"}");
        return http_build_response(HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE,
                                   HTTP_CONTENT_TYPE_APPLICATION_JSON,
                                   byte_view_from_str(body), keep_alive,
                                   response_buf);
    }

    /* Empty file: return 200 with zero-length body. */
    if (file_size == 0) {
        struct byte_view empty = byte_view_new(NULL, 0);
        return http_build_response(HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_PLAIN,
                                   empty, keep_alive, response_buf);
    }

    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error) {
        struct str body = STR("{\"error\":\"open failed\"}");
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), keep_alive, response_buf);
    }

    struct vfs_file f = result_vfs_file_checked(fres);
    char *buf = arena_alloc(&tmp, file_size);
    struct byte_buf bb = byte_buf_new(buf, 0, file_size);
    struct result_sz rr = vfs_read(&f, &bb, 0);
    vfs_close(&f);

    if (rr.is_error) {
        struct str body = STR("{\"error\":\"read failed\"}");
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), keep_alive, response_buf);
    }

    struct byte_view content = byte_view_new(bb.dat, bb.len);
    return http_build_response(HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_PLAIN,
                               content, keep_alive, response_buf);
}

/* /api/fs?path=X - directory listing as JSON. */

static struct result http_serve_api_fs(struct str query,
                                       bool keep_alive,
                                       struct byte_buf *response_buf,
                                       struct arena tmp)
{
    assert(response_buf);

    struct str path = http_query_get_param(query, STR("path"));
    if (path.len == 0)
        path = STR("/");

    path = percent_decode(path, &tmp);

    /* Require an absolute path. */
    if (path.len == 0 || path.dat[0] != '/') {
        struct str body = STR("{\"error\":\"invalid path\"}");
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), false, response_buf);
    }

    struct result_vfs_stat st = vfs_stat(path);
    if (st.is_error || result_vfs_stat_checked(st).type != VFS_TYPE_DIR) {
        struct str body = STR("{\"error\":\"not found\"}");
        return http_build_response(
            HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TYPE_APPLICATION_JSON,
            byte_view_from_str(body), keep_alive, response_buf);
    }

    struct str_buf sbuf = str_buf_from_arena(&tmp, 2048);
    str_buf_append(&sbuf, STR("{\"path\":"));
    struct byte_buf jbuf = byte_buf_from_str_buf(sbuf);
    json_emit_string(&jbuf, path);
    sbuf = str_buf_from_byte_buf(jbuf);
    str_buf_append(&sbuf, STR(",\"entries\":["));

    sz idx = 0;
    for (;;) {
        struct result_vfs_dirent de = vfs_readdir(path, idx);
        if (de.is_error)
            break;
        struct vfs_dirent ent = result_vfs_dirent_checked(de);
        if (idx > 0)
            str_buf_append_char(&sbuf, ',');
        str_buf_append(&sbuf, STR("{\"name\":"));
        jbuf = byte_buf_from_str_buf(sbuf);
        json_emit_string(&jbuf, ent.name);
        sbuf = str_buf_from_byte_buf(jbuf);
        fmt(&sbuf, STR(",\"type\":\"%s\"}"),
            ent.type == VFS_TYPE_DIR ? STR("dir") : STR("file"));
        idx++;
    }

    str_buf_append(&sbuf, STR("]}"));

    return http_build_response(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_APPLICATION_JSON,
        byte_view_from_str(str_from_buf(sbuf)), keep_alive, response_buf);
}

static struct result http_handle_request(struct ram_fs_node *root,
                                         struct str request_data,
                                         bool *keep_alive_out,
                                         struct byte_buf *response_buf,
                                         struct byte_view *out_file_body,
                                         struct arena tmp)
{
    assert(root);
    assert(response_buf);
    assert(keep_alive_out);

    struct http_request req = http_parse_request(request_data);

    if (!req.valid) {
        pr_info(STR("Received invalid HTTP request: %s\n"),
                http_request_header_str(request_data, tmp));
        *keep_alive_out = false;
        return http_build_response(
            HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(bad_request_body), false, response_buf);
    }

    bool keep_alive = http_request_keep_alive(request_data, req.version);
    *keep_alive_out = keep_alive;

    /* Task 30: reject oversized request paths immediately (414 URI Too Long).
     */
    if (req.path.len > WEB_MAX_REQUEST_PATH) {
        pr_warn(STR("Request path too long (%ld > %d). Returning 414.\n"),
                req.path.len, WEB_MAX_REQUEST_PATH);
        *keep_alive_out = false;
        return http_build_response(
            HTTP_STATUS_URI_TOO_LONG, HTTP_CONTENT_TYPE_TEXT_HTML,
            byte_view_from_str(bad_request_body), false, response_buf);
    }

    pr_info(STR("Handling HTTP request: %s %s %s\n"),
            http_method_to_string(req.method), req.path,
            http_version_to_string(req.version));

    if (str_is_equal(req.path, STR("/mazu-stats")))
        return http_serve_stats(keep_alive, response_buf, tmp);

#if CONFIG_DEBUG_ENDPOINT
    if (str_is_equal(req.path, STR("/debug")) && req.method == HTTP_METHOD_GET)
        return http_serve_debug(keep_alive, response_buf, tmp);
#endif

    /* Shell API routes. */
    struct str query;
    struct str path = http_split_path_query(req.path, &query);

    if (str_is_equal(path, STR("/api/shell/in")) &&
        (req.method == HTTP_METHOD_GET || req.method == HTTP_METHOD_POST)) {
        /* Shell polling is bursty and competes with the fixed 8-slot web
         * server. Do not keep these connections idle between requests.
         */
        *keep_alive_out = false;
        struct str body = str_new(NULL, 0);
        if (req.method == HTTP_METHOD_POST) {
            body = http_get_body(request_data);
            /* Clamp body to Content-Length so pipelined bytes from the next
             * request on a keep-alive connection are not consumed as body.
             */
            sz cl = 0;
            if (!http_parse_content_length(request_data, &cl))
                return http_build_response(
                    HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_HTML,
                    byte_view_from_str(bad_request_body), false, response_buf);
            if (body.len > cl)
                body.len = cl;
        }
        return http_serve_shell_in(query, body, false, response_buf, tmp);
    }

    if (str_is_equal(path, STR("/api/shell/out")) &&
        req.method == HTTP_METHOD_GET) {
        *keep_alive_out = false;
        return http_serve_shell_out(query, false, response_buf, tmp);
    }

    /* JSON API routes. */
    if (str_is_equal(path, STR("/api/stats")) && req.method == HTTP_METHOD_GET)
        return http_serve_api_stats(keep_alive, response_buf, tmp);

    if (str_is_equal(path, STR("/api/tcp")) && req.method == HTTP_METHOD_GET)
        return http_serve_api_tcp(keep_alive, response_buf, tmp);

    if (str_is_equal(path, STR("/api/arp")) && req.method == HTTP_METHOD_GET)
        return http_serve_api_arp(keep_alive, response_buf, tmp);

    if (str_is_equal(path, STR("/api/fs/read")) &&
        req.method == HTTP_METHOD_GET)
        return http_serve_api_fs_read(query, keep_alive, response_buf, tmp);

    if (str_is_equal(path, STR("/api/fs")) && req.method == HTTP_METHOD_GET)
        return http_serve_api_fs(query, keep_alive, response_buf, tmp);

#if CONFIG_WEB_TELEMETRY
    if (str_is_equal(path, STR("/api/klog")) && req.method == HTTP_METHOD_GET)
        return http_serve_api_klog(keep_alive, response_buf, tmp);
#endif

    /* RFC 7231 §4: static file handler only supports GET.
     * POST with If-None-Match would otherwise incorrectly return 304 instead of
     * 412.
     */
    if (req.method != HTTP_METHOD_GET)
        return http_build_response(HTTP_STATUS_METHOD_NOT_ALLOWED,
                                   HTTP_CONTENT_TYPE_TEXT_HTML,
                                   byte_view_from_str(method_not_allowed_body),
                                   keep_alive, response_buf);

    /* Task 32: extract If-None-Match for conditional GET (ETag). */
    struct str if_none_match = http_header_value(request_data, STR("If-None-"
                                                                   "Match"));

    /* Allocate scratch buffer for URL normalization (Task 29).
     * Size is WEB_MAX_REQUEST_PATH + 1 to allow writing '\0' for safety.
     */
    char *norm_buf = arena_alloc(&tmp, WEB_MAX_REQUEST_PATH + 1);

    return http_serve_file(root, path, if_none_match, keep_alive, norm_buf,
                           WEB_MAX_REQUEST_PATH, response_buf, out_file_body);
}

static bool http_is_complete_header(struct str request_data)
{
    return !str_find_substring(request_data, STR("\r\n\r\n")).is_none;
}

/* Connection handling

 * Explicit header-size cap (item 19): 8 KB is enough for any reasonable
 * request, including large cookie and query-string headers.
 */
#define WEB_MAX_REQUEST_HEADER 8192
/* Total timeout for receiving a complete request header (item 19). */
#define WEB_REQUEST_TIMEOUT_MS 5000
/* Per-route POST body cap for shell commands (item 19). */
#define WEB_SHELL_POST_BODY_MAX 4096

/* Extra arena space per connection beyond WEB_MAX_RESPONSE_SIZE.
 * This covers request parsing, shell output formatting, URL normalization,
 * debug responses, klog formatting, tcp_conn_format scratch space, and small
 * alignment and error-path buffers.
 */
#if CONFIG_WEB_TELEMETRY
/* /api/klog allocates KLOG_SIZE (peek) + KLOG_SIZE*6+256 (JSON escape). */
#define WEB_KLOG_OVERHEAD (KLOG_SIZE * 7 + 256)
#else
#define WEB_KLOG_OVERHEAD 0
#endif
#define WEB_CONN_ARENA_OVERHEAD                                                \
    (WEB_MAX_REQUEST_HEADER + SHELL_OUT_BUF_SIZE + 64 + WEB_MAX_REQUEST_PATH + \
     1 + WEB_KLOG_OVERHEAD + 4096)

#define WEB_NUM_RECV_RETRIES 10
#define WEB_MAX_RESPONSE_SIZE BIT(22) /* 4 MiB */
#define WEB_MAX_CONNS 8
#define WEB_KEEPALIVE_TIMEOUT_MS 30000

/* Poll the TCP module for newly received data and store it in 'recv_buf'.
 * Used by the WebSocket path which needs a multi-retry polling loop.
 */
static struct result_sz web_recv_retry(struct tcp_conn *conn,
                                       struct byte_buf *recv_buf)
{
    assert(conn);
    assert(recv_buf);

    sz n_received = 0;
    bool peer_closed_conn = false;

    for (sz i = 0; i < WEB_NUM_RECV_RETRIES; i++) {
        struct result_sz res = tcp_conn_recv(conn, recv_buf, &peer_closed_conn);
        if (res.is_error)
            return result_sz_error(res.code);

        n_received = result_sz_checked(res);
        if (n_received)
            break;

        if (peer_closed_conn)
            break;

        sleep_ms(time_ms_new(10));
    }

    return result_sz_ok(n_received);
}

/* Send 'response' over 'conn' without closing the connection.
 * Returns an error if the connection is lost mid-send.
 * Used by the WebSocket path for handshake and frame transmission.
 */
static struct result web_respond(struct tcp_conn *conn,
                                 struct byte_view response,
                                 struct arena tmp)
{
    sz n_transmitted = 0;
    bool peer_closed_conn = false;

    while (!peer_closed_conn) {
        struct byte_view transmit = byte_view_skip(response, n_transmitted);
        struct result_sz res =
            tcp_conn_send(conn, transmit, &peer_closed_conn, tmp);
        if (res.is_error)
            return result_error(res.code);

        n_transmitted += result_sz_checked(res);
        if (n_transmitted >= response.len)
            break;

        sleep_ms(time_ms_new(10));
    }

    if (n_transmitted < response.len)
        return result_error(ECONNRESET);

    return result_ok();
}

/* WebSocket upgrade path (item 17, CONFIG_WEBSOCKET) */

#if CONFIG_WEBSOCKET

/* WebSocket opcodes (RFC 6455 §11.8). */
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

/* Maximum client-frame payload accepted before sending CLOSE 1009. */
#define WS_MAX_FRAME_PAYLOAD 4096

/* RFC 6455 §1.3: magic GUID appended to the client key before SHA-1. */
#define WS_ACCEPT_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

struct ws_frame {
    bool fin;
    u8 opcode;
    bool masked;
    u64 payload_len;
    u8 mask[4];
    byte *payload; /* points into the frame receive buffer; already unmasked */
};

/* Return true if the request contains a valid WebSocket upgrade intent. */
static bool ws_is_upgrade_request(struct str request_data)
{
    return http_header_has_token(request_data, STR("Connection"),
                                 STR("Upgrade")) &&
           http_str_is_equal_ci(http_header_value(request_data, STR("Upgrade")),
                                STR("websocket"));
}

/* Compute the Sec-WebSocket-Accept value:
 *   base64( sha1( client_key || WS_ACCEPT_GUID ) )
 * Writes exactly BASE64_ENCODED_SIZE(SHA1_DIGEST_SIZE) = 28 chars into
 * accept_out (no NUL terminator) and returns the length (28).
 */
static sz ws_compute_accept_key(struct str key_str, char accept_out[28])
{
    /* key <= 24 chars; GUID = 36 chars; total <= 60 bytes; fits in buf[64]. */
    char buf[64];
    sz buf_len = 0;

    for (sz i = 0; i < key_str.len && buf_len < 60; i++)
        buf[buf_len++] = key_str.dat[i];
    for (sz i = 0; WS_ACCEPT_GUID[i] != '\0' && buf_len < 60; i++)
        buf[buf_len++] = WS_ACCEPT_GUID[i];

    u8 digest[SHA1_DIGEST_SIZE];
    sha1((const u8 *) buf, buf_len, digest);
    return base64_encode(digest, SHA1_DIGEST_SIZE, accept_out);
}

/* Receive data into 'buf' until at least 'needed' bytes are present.
 * Returns EIO if the peer closes before delivering enough bytes.
 */
static struct result ws_recv_at_least(struct tcp_conn *conn,
                                      struct byte_buf *buf,
                                      sz needed)
{
    for (sz retry = 0; buf->len < needed && retry < 50; retry++) {
        struct result_sz res = web_recv_retry(conn, buf);
        if (res.is_error)
            return result_error(res.code);
        if (!result_sz_checked(res))
            sleep_ms(time_ms_new(10));
    }
    return (buf->len >= needed) ? result_ok() : result_error(EIO);
}

/* Receive one complete WebSocket frame into 'frame_buf' and fill 'frame'.
 * The payload bytes inside 'frame_buf' are unmasked in-place.
 * Returns EMSGSIZE for oversized payloads and EINVAL for protocol violations.
 */
static struct result ws_frame_recv(struct tcp_conn *conn,
                                   struct byte_buf *frame_buf,
                                   struct ws_frame *frame)
{
    struct result res = ws_recv_at_least(conn, frame_buf, 2);
    if (res.is_error)
        return res;

    byte *raw = frame_buf->dat;
    frame->fin = (raw[0] >> 7) & 1;
    frame->opcode = raw[0] & 0x0f;
    frame->masked = (raw[1] >> 7) & 1;
    u64 len7 = raw[1] & 0x7f;
    sz hdr_end = 2;

    if (len7 < 126) {
        frame->payload_len = len7;
    } else if (len7 == 126) {
        res = ws_recv_at_least(conn, frame_buf, 4);
        if (res.is_error)
            return res;
        raw = frame_buf->dat;
        frame->payload_len = ((u64) raw[2] << 8) | raw[3];
        hdr_end = 4;
    } else { /* len7 == 127: 8-byte extended length */
        res = ws_recv_at_least(conn, frame_buf, 10);
        if (res.is_error)
            return res;
        raw = frame_buf->dat;
        frame->payload_len = 0;
        for (sz i = 0; i < 8; i++)
            frame->payload_len = (frame->payload_len << 8) | raw[2 + i];
        hdr_end = 10;
    }

    if (frame->masked) {
        res = ws_recv_at_least(conn, frame_buf, hdr_end + 4);
        if (res.is_error)
            return res;
        raw = frame_buf->dat;
        for (sz i = 0; i < 4; i++)
            frame->mask[i] = raw[hdr_end + i];
        hdr_end += 4;
    }

    if (!frame->masked)
        return result_error(EINVAL);

    /* Reject oversized frames (CLOSE 1009 sent by the caller). */
    if (frame->payload_len > WS_MAX_FRAME_PAYLOAD)
        return result_error(EMSGSIZE);

    res = ws_recv_at_least(conn, frame_buf, hdr_end + (sz) frame->payload_len);
    if (res.is_error)
        return res;

    raw = frame_buf->dat;
    frame->payload = raw + hdr_end;

    /* Unmask payload in-place (client frames MUST be masked, RFC 6455 section
     * 5.3).
     */
    if (frame->masked) {
        for (sz i = 0; i < (sz) frame->payload_len; i++)
            frame->payload[i] ^= frame->mask[i & 3];
    }

    return result_ok();
}

/* Send one WebSocket frame (server-to-client: no masking per RFC 6455 §5.1). */
static struct result ws_frame_send(struct tcp_conn *conn,
                                   u8 opcode,
                                   const byte *payload,
                                   sz payload_len,
                                   struct arena tmp)
{
    struct str_buf sbuf = str_buf_from_arena(&tmp, 10 + payload_len);
    struct result r;

    r = str_buf_append_char(&sbuf, (char) (0x80u | opcode)); /* FIN=1, opcode */
    if (r.is_error)
        return r;
    if (payload_len < 126) {
        r = str_buf_append_char(&sbuf, (char) payload_len);
    } else if (payload_len < 65536) {
        r = str_buf_append_char(&sbuf, 126);
        if (!r.is_error)
            r = str_buf_append_char(&sbuf, (char) ((u32) payload_len >> 8));
        if (!r.is_error)
            r = str_buf_append_char(&sbuf, (char) ((u32) payload_len & 0xff));
    } else {
        r = str_buf_append_char(&sbuf, 127);
        for (int i = 7; i >= 0 && !r.is_error; i--)
            r = str_buf_append_char(
                &sbuf, (char) (((u64) payload_len >> (i * 8)) & 0xff));
    }
    if (r.is_error)
        return r;

    for (sz i = 0; i < payload_len; i++) {
        r = str_buf_append_char(&sbuf, (char) payload[i]);
        if (r.is_error)
            return r;
    }

    return web_respond(conn, byte_view_from_str(str_from_buf(sbuf)), tmp);
}

/* Send "101 Switching Protocols" and complete the RFC 6455 handshake.
 * Returns an error (without closing the connection) if the upgrade key is
 * absent or if the 101 response cannot be transmitted; the caller closes.
 */
static struct result ws_handshake(struct tcp_conn *conn,
                                  struct str request_data,
                                  struct arena tmp)
{
    struct str key = http_header_value(request_data, STR("Sec-WebSocket-Key"));
    if (key.len == 0) {
        /* RFC 6455 §4.2.1 requires the key. Send 400 and let the caller close.
         */
        struct byte_buf rbuf =
            byte_buf_from_array(byte_array_from_arena(1280, &tmp));
        http_build_response(HTTP_STATUS_BAD_REQUEST,
                            HTTP_CONTENT_TYPE_TEXT_HTML,
                            byte_view_from_str(bad_request_body), false, &rbuf);
        web_respond(conn, byte_view_from_buf(rbuf), tmp);
        return result_error(EINVAL);
    }

    char accept[28]; /* BASE64_ENCODED_SIZE(SHA1_DIGEST_SIZE) = 28 */
    sz accept_len = ws_compute_accept_key(key, accept);

    struct str_buf sbuf = str_buf_from_arena(&tmp, 256);
    struct result r;
    r = str_buf_append(&sbuf, STR("HTTP/1.1 101 Switching Protocols\r\n"));
    if (!r.is_error)
        r = str_buf_append(&sbuf, STR("Upgrade: websocket\r\n"));
    if (!r.is_error)
        r = str_buf_append(&sbuf, STR("Connection: Upgrade\r\n"));
    if (!r.is_error)
        r = str_buf_append(&sbuf, STR("Sec-WebSocket-Accept: "));
    if (!r.is_error)
        r = str_buf_append(&sbuf, str_new(accept, accept_len));
    if (!r.is_error)
        r = str_buf_append(&sbuf, STR("\r\n\r\n"));
    if (r.is_error)
        return r;

    return web_respond(conn, byte_view_from_str(str_from_buf(sbuf)), tmp);
}

/* Run the WebSocket message loop (WCS_WS_ACTIVE).
 *
 * Control frames: PING → PONG (RFC 6455 §5.5.3); CLOSE → echo + close TCP.
 * Data frames: unfragmented TEXT/BINARY are echoed back.
 * Fragmentation: not supported → CLOSE 1003 Unsupported Data.
 * Oversized frames: CLOSE 1009 Message Too Big.
 * Unknown opcodes: CLOSE 1002 Protocol Error.
 *
 * Always closes the TCP connection before returning.
 */
static struct result ws_serve(struct tcp_conn *conn, struct arena tmp)
{
    /* Allocate the frame receive buffer once, before entering the loop so
     * per-frame ws_frame_send calls (which take 'tmp' by value) don't compete
     * with it for arena space.
     */
    struct byte_buf frame_buf = byte_buf_from_array(
        byte_array_from_arena(10 + WS_MAX_FRAME_PAYLOAD + 4, &tmp));

    /* RFC 6455 close-frame status codes (big-endian u16). */
    static const byte close_1002[2] = {0x03, 0xEA}; /* Protocol Error */
    static const byte close_1003[2] = {0x03, 0xEB}; /* Unsupported Data */
    static const byte close_1009[2] = {0x03, 0xF1}; /* Message Too Big  */

    for (;;) {
        frame_buf.len = 0;

        struct ws_frame frame;
        struct result res = ws_frame_recv(conn, &frame_buf, &frame);
        if (res.is_error) {
            if (res.code == EMSGSIZE)
                ws_frame_send(conn, WS_OP_CLOSE, close_1009, 2, tmp);
            else if (res.code == EINVAL)
                ws_frame_send(conn, WS_OP_CLOSE, close_1002, 2, tmp);
            tcp_conn_close(&conn, tmp);
            return res;
        }

        switch (frame.opcode) {
        case WS_OP_PING:
            /* Must reply with Pong carrying the same payload
             * (RFC 6455 section 5.5.3).
             */
            res = ws_frame_send(conn, WS_OP_PONG, frame.payload,
                                (sz) frame.payload_len, tmp);
            if (res.is_error) {
                tcp_conn_close(&conn, tmp);
                return res;
            }
            break;

        case WS_OP_CLOSE:
            /* Echo the close frame (preserving status code + reason), then
             * close.
             */
            ws_frame_send(conn, WS_OP_CLOSE, frame.payload,
                          (sz) frame.payload_len, tmp);
            tcp_conn_close(&conn, tmp);
            return result_ok();

        case WS_OP_TEXT:
        case WS_OP_BINARY:
            if (!frame.fin) {
                /* Fragmented messages are not supported. */
                ws_frame_send(conn, WS_OP_CLOSE, close_1003, 2, tmp);
                tcp_conn_close(&conn, tmp);
                return result_ok();
            }
            res = ws_frame_send(conn, frame.opcode, frame.payload,
                                (sz) frame.payload_len, tmp);
            if (res.is_error) {
                tcp_conn_close(&conn, tmp);
                return res;
            }
            break;

        case WS_OP_CONTINUATION:
            /* Continuation without a preceding data frame is a protocol error.
             */
            ws_frame_send(conn, WS_OP_CLOSE, close_1003, 2, tmp);
            tcp_conn_close(&conn, tmp);
            return result_ok();

        default:
            ws_frame_send(conn, WS_OP_CLOSE, close_1002, 2, tmp);
            tcp_conn_close(&conn, tmp);
            return result_ok();
        }
    }
}

#endif /* CONFIG_WEBSOCKET */

/* Event-loop multi-connection server

 * Per-connection state machine.
 *
 * Flow:
 *
 *     WCS_RECV
 *        |
 *        +-> WCS_RECV_BODY   when POST body is incomplete
 *        |
 *        v
 *     WCS_HANDLE
 *        |
 *        +-> WCS_WS_ACTIVE   for WebSocket upgrades
 *        +-> WCS_SSE_DONE    for the direct SSE test path
 *        |
 *        v
 *      WCS_SEND
 *        |
 *        +-> WCS_KEEPALIVE   after a complete keep-alive response
 *        |
 *        v
 *      free connection
 *
 * Each transition is explicit, so request parsing must finish before the send
 * path is reachable.
 */
enum web_conn_state {
    WCS_RECV,      /* receiving and buffering the HTTP request header */
    WCS_RECV_BODY, /* receiving POST body after header is complete */
    WCS_HANDLE,    /* parsing headers, building the response */
    WCS_SEND,      /* transmitting the response */
    WCS_KEEPALIVE, /* keep-alive: loop back to WCS_RECV for next request */
#if CONFIG_WEBSOCKET
    WCS_WS_ACTIVE, /* WebSocket connection active: frame codec loop */
#endif
    WCS_SSE_DONE, /* SSE stream completed, ready to free */
};

struct web_conn {
    struct tcp_conn *tcp; /* NULL marks an idle slot */
    enum web_conn_state state;

    /* Receive */
    struct byte_buf recv_buf;
    u64 deadline_ms;
    sz content_length; /* expected POST body length (WCS_RECV_BODY) */

    /* Response */
    struct byte_buf response_buf;
    struct byte_view file_body; /* zero-copy: points directly to ramfs data */
    sz send_offset;
    bool keep_alive;

    /* Memory: own arena per connection, kvalloc-allocated. */
    struct arena arena;
    byte *arena_base;
    sz arena_size;
    byte *arena_checkpoint; /* reset point between keep-alive requests */

    /* Timing */
    u64 last_activity_ms;
};

struct web_server {
    struct tcp_conn *listen_conn;
    struct ram_fs_node *root;
    struct web_conn conns[WEB_MAX_CONNS];
};

/* Allocate a connection slot and per-connection arena.
 * Returns NULL if no slot is available or kvalloc fails.
 */
static struct web_conn *web_conn_alloc(struct web_server *srv,
                                       struct tcp_conn *tcp)
{
    struct web_conn *wc = NULL;
    for (sz i = 0; i < WEB_MAX_CONNS; i++) {
        if (!srv->conns[i].tcp) {
            wc = &srv->conns[i];
            break;
        }
    }
    if (!wc)
        return NULL;

    sz arena_sz = WEB_CONN_ARENA_OVERHEAD + WEB_MAX_RESPONSE_SIZE;
    struct option_byte_array mem = kvalloc_alloc(arena_sz, 64);
    if (mem.is_none)
        return NULL;

    struct byte_array ba = option_byte_array_checked(mem);
    wc->arena_base = ba.dat;
    wc->arena_size = ba.len;
    wc->arena = arena_new(ba);
    wc->arena_checkpoint = wc->arena.beg;

    wc->recv_buf = byte_buf_from_array(
        byte_array_from_arena(WEB_MAX_REQUEST_HEADER, &wc->arena));
    wc->state = WCS_RECV;
    wc->send_offset = 0;
    wc->file_body = byte_view_new(NULL, 0);
    wc->keep_alive = false;
    wc->content_length = 0;
    wc->deadline_ms = time_current_ms().ms + WEB_REQUEST_TIMEOUT_MS;
    wc->last_activity_ms = time_current_ms().ms;

    /* Set tcp last: this marks the slot as active. */
    wc->tcp = tcp;

    return wc;
}

/* Release a connection: close TCP, free arena, zero the slot. */
static void web_conn_free(struct web_conn *wc)
{
    if (wc->tcp)
        tcp_conn_close(&wc->tcp, wc->arena);
    if (wc->arena_base) {
        kvalloc_free(byte_array_new(wc->arena_base, wc->arena_size));
        wc->arena_base = NULL;
        wc->arena_size = 0;
    }
    wc->tcp = NULL;
}

/* Reset a keep-alive connection for the next request cycle. */
static void web_conn_reset_keepalive(struct web_conn *wc)
{
    wc->arena.beg = wc->arena_checkpoint;
    wc->recv_buf = byte_buf_from_array(
        byte_array_from_arena(WEB_MAX_REQUEST_HEADER, &wc->arena));
    wc->state = WCS_RECV;
    wc->send_offset = 0;
    wc->file_body = byte_view_new(NULL, 0);
    wc->keep_alive = false;
    wc->content_length = 0;
    wc->deadline_ms = time_current_ms().ms + WEB_REQUEST_TIMEOUT_MS;
    wc->last_activity_ms = time_current_ms().ms;
}

/* SSE test endpoint: streams three SSE events in chunked encoding, then
 * closes.  Used by integration tests to validate chunked transfer and
 * Server-Sent Events plumbing.
 */
static struct result sse_test_handler(struct tcp_conn *conn, struct arena tmp)
{
    byte hdr_mem[256];
    struct byte_buf hdr = byte_buf_new(hdr_mem, 0, sizeof(hdr_mem));
    struct result res = http_build_chunked_header(
        HTTP_STATUS_OK, HTTP_CONTENT_TYPE_TEXT_EVENT_STREAM, &hdr);
    if (res.is_error)
        return res;

    res = web_respond(conn, byte_view_new((char *) hdr.dat, hdr.len), tmp);
    if (res.is_error)
        return res;

    /* Send three test events. */
    for (i32 i = 0; i < 3; i++) {
        byte ev_mem[64];
        struct byte_buf ev = byte_buf_new(ev_mem, 0, sizeof(ev_mem));
        struct str_buf sb = str_buf_from_byte_buf(ev);
        fmt(&sb, STR("data: {\"n\":%d}\n\n"), i);
        ev = byte_buf_from_str_buf(sb);

        res = web_send_chunk(conn, byte_view_new((char *) ev.dat, ev.len), tmp);
        if (res.is_error)
            return res;
    }

    return web_end_chunked(conn, tmp);
}

/* Advance one connection by a single non-blocking step.
 * Returns true if progress was made (data received, response sent, state
 * change).
 */
static bool web_step_conn(struct web_server *srv, struct web_conn *wc)
{
    switch (wc->state) {
    case WCS_RECV: {
        bool peer_closed = false;
        struct result_sz res =
            tcp_conn_recv(wc->tcp, &wc->recv_buf, &peer_closed);
        if (res.is_error) {
            web_conn_free(wc);
            return true;
        }

        sz n = result_sz_checked(res);

        /* Peer closed without sending anything. */
        if (peer_closed && wc->recv_buf.len == 0) {
            web_conn_free(wc);
            return true;
        }

        /* Header-size overflow: send 431 and close (Slowloris defense). */
        if (wc->recv_buf.len >= wc->recv_buf.cap) {
            wc->response_buf =
                byte_buf_from_array(byte_array_from_arena(1280, &wc->arena));
            http_build_response(HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE,
                                HTTP_CONTENT_TYPE_TEXT_HTML,
                                byte_view_from_str(request_too_large_body),
                                false, &wc->response_buf);
            wc->keep_alive = false;
            wc->state = WCS_SEND;
            pr_warn(STR("Request header exceeds %lu bytes. Sending 431.\n"),
                    (u64) WEB_MAX_REQUEST_HEADER);
            return true;
        }

        /* Complete HTTP header received. */
        if (http_is_complete_header(str_from_byte_buf(wc->recv_buf))) {
            wc->state = WCS_HANDLE;
            return true;
        }

        /* Peer closed mid-header: nothing more to read. */
        if (peer_closed) {
            web_conn_free(wc);
            return true;
        }

        /* Timeout: if no data arrived this is just an idle keep-alive
         * connection - close it silently.  Only send 400 when partial
         * header data was received (genuinely broken request).
         */
        if (time_current_ms().ms >= wc->deadline_ms) {
            if (wc->recv_buf.len == 0) {
                web_conn_free(wc);
                return true;
            }
            wc->response_buf =
                byte_buf_from_array(byte_array_from_arena(1280, &wc->arena));
            http_build_response(
                HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_HTML,
                byte_view_from_str(bad_request_body), false, &wc->response_buf);
            wc->keep_alive = false;
            wc->state = WCS_SEND;
            pr_warn(STR("Request header timeout. Sending 400.\n"));
            return true;
        }

        if (n > 0)
            wc->last_activity_ms = time_current_ms().ms;
        return n > 0;
    }

    case WCS_RECV_BODY: {
        bool peer_closed = false;
        struct result_sz res =
            tcp_conn_recv(wc->tcp, &wc->recv_buf, &peer_closed);
        if (res.is_error) {
            web_conn_free(wc);
            return true;
        }

        sz n = result_sz_checked(res);

        /* Check if the full POST body has arrived.  Do this BEFORE
         * inspecting peer_closed: the final data and FIN can arrive
         * in the same segment, so the body may already be complete.
         */
        struct str body = http_get_body(str_from_byte_buf(wc->recv_buf));
        if ((sz) body.len >= wc->content_length) {
            wc->state = WCS_HANDLE;
            return true;
        }

        /* Body still incomplete and peer closed; nothing more to read. */
        if (peer_closed) {
            web_conn_free(wc);
            return true;
        }

        /* Timeout waiting for body. */
        if (time_current_ms().ms >= wc->deadline_ms) {
            wc->response_buf =
                byte_buf_from_array(byte_array_from_arena(1280, &wc->arena));
            http_build_response(
                HTTP_STATUS_BAD_REQUEST, HTTP_CONTENT_TYPE_TEXT_HTML,
                byte_view_from_str(bad_request_body), false, &wc->response_buf);
            wc->keep_alive = false;
            wc->state = WCS_SEND;
            return true;
        }

        if (n > 0)
            wc->last_activity_ms = time_current_ms().ms;
        return n > 0;
    }

    case WCS_HANDLE: {
        struct str req_str = str_from_byte_buf(wc->recv_buf);

#if CONFIG_WEBSOCKET
        /* WebSocket upgrade: intercept before normal HTTP routing. */
        if (ws_is_upgrade_request(req_str)) {
            struct result hs_res = ws_handshake(wc->tcp, req_str, wc->arena);
            if (hs_res.is_error) {
                web_conn_free(wc);
                return true;
            }
            wc->state = WCS_WS_ACTIVE;
            return true;
        }
#endif

        /* SSE test endpoint: intercept before buffered response path.
         * Streams directly on the TCP connection (like WebSocket).
         */
        {
            struct http_request sse_peek = http_parse_request(req_str);
            if (sse_peek.valid && str_is_equal(sse_peek.path, STR("/api/sse/"
                                                                  "test"))) {
                sse_test_handler(wc->tcp, wc->arena);
                wc->state = WCS_SSE_DONE;
                return true;
            }
        }

        /* Buffer the POST body; enforce per-route size cap. */
        struct http_request peek = http_parse_request(req_str);
        if (peek.valid && peek.method == HTTP_METHOD_POST) {
            sz cl = 0;
            if (!http_parse_content_length(req_str, &cl)) {
                wc->response_buf = byte_buf_from_array(
                    byte_array_from_arena(1280, &wc->arena));
                http_build_response(HTTP_STATUS_BAD_REQUEST,
                                    HTTP_CONTENT_TYPE_TEXT_HTML,
                                    byte_view_from_str(bad_request_body), false,
                                    &wc->response_buf);
                wc->keep_alive = false;
                wc->state = WCS_SEND;
                return true;
            }
            if (cl > WEB_SHELL_POST_BODY_MAX) {
                wc->response_buf = byte_buf_from_array(
                    byte_array_from_arena(1280, &wc->arena));
                http_build_response(
                    HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE,
                    HTTP_CONTENT_TYPE_TEXT_HTML,
                    byte_view_from_str(post_body_too_large_body), false,
                    &wc->response_buf);
                wc->keep_alive = false;
                wc->state = WCS_SEND;
                return true;
            }
            struct str body = http_get_body(req_str);
            if ((sz) body.len < cl) {
                /* Body not yet fully received; continue in WCS_RECV_BODY. */
                wc->content_length = cl;
                wc->state = WCS_RECV_BODY;
                return true;
            }
        }

        /* Build the HTTP response. */
        wc->response_buf = byte_buf_from_array(
            byte_array_from_arena(WEB_MAX_RESPONSE_SIZE, &wc->arena));
        wc->file_body = byte_view_new(NULL, 0);
        struct result http_res =
            http_handle_request(srv->root, req_str, &wc->keep_alive,
                                &wc->response_buf, &wc->file_body, wc->arena);
        if (http_res.is_error) {
            if (http_res.code == ENOMEM) {
                /* Response buffer overflow: the file is too large. Send 507. */
                wc->response_buf.len = 0;
                wc->file_body = byte_view_new(NULL, 0);
                http_build_response(
                    HTTP_STATUS_INSUFFICIENT_STORAGE,
                    HTTP_CONTENT_TYPE_TEXT_HTML,
                    byte_view_from_str(insufficient_storage_body), false,
                    &wc->response_buf);
                wc->keep_alive = false;
            } else {
                web_conn_free(wc);
                return true;
            }
        }
        wc->send_offset = 0;
        wc->state = WCS_SEND;
        return true;
    }

    case WCS_SEND: {
        /* Two-phase send: response_buf (headers, or full response for API
         * endpoints) followed by file_body (zero-copy ramfs data, only set
         * for static file serving).  send_offset spans the combined length.
         */
        sz hdr_len = (sz) wc->response_buf.len;
        sz total_len = hdr_len + wc->file_body.len;
        struct byte_view remaining;

        if (wc->send_offset < hdr_len) {
            /* Still sending headers from response_buf. */
            struct byte_view full = byte_view_from_buf(wc->response_buf);
            remaining = byte_view_skip(full, wc->send_offset);
        } else {
            /* Sending file body directly from ramfs (zero-copy). */
            sz body_offset = wc->send_offset - hdr_len;
            remaining = byte_view_skip(wc->file_body, body_offset);
        }

        bool peer_closed = false;
        struct result_sz res =
            tcp_conn_send(wc->tcp, remaining, &peer_closed, wc->arena);
        if (res.is_error || peer_closed) {
            web_conn_free(wc);
            return true;
        }

        sz sent = result_sz_checked(res);
        wc->send_offset += sent;

        if (sent > 0)
            wc->last_activity_ms = time_current_ms().ms;

        if (wc->send_offset >= total_len) {
            /* Fully sent. */
            if (wc->keep_alive) {
                wc->state = WCS_KEEPALIVE;
                return true;
            }
            web_conn_free(wc);
            return true;
        }

        return sent > 0;
    }

    case WCS_KEEPALIVE:
        web_conn_reset_keepalive(wc);
        return true;

#if CONFIG_WEBSOCKET
    case WCS_WS_ACTIVE:
        /* Blocking: ws_serve closes the TCP connection before returning. */
        ws_serve(wc->tcp, wc->arena);
        wc->tcp = NULL; /* ws_serve already closed it */
        web_conn_free(wc);
        return true;
#endif

    case WCS_SSE_DONE:
        web_conn_free(wc);
        return true;
    }

    return false; /* unreachable */
}

struct result web_listen(struct ipv4_addr ip_addr,
                         u16 port,
                         struct ram_fs_node *root)
{
    struct web_server srv = {0};
    struct arena setup =
        arena_new(option_byte_array_checked(kvalloc_alloc(4096, 64)));
    srv.listen_conn = tcp_conn_listen(ip_addr, port, setup);
    srv.root = root;

    pr_info(STR("Listening for connections on %s:%hu\n"),
            ipv4_addr_format(ip_addr, &setup), port);

    for (;;) {
        bool progress = false;

        /* Accept new connections (non-blocking). */
        while (tcp_conn_has_pending(srv.listen_conn)) {
            struct tcp_conn *tcp = tcp_conn_accept(srv.listen_conn);
            if (!tcp)
                break;
            if (!web_conn_alloc(&srv, tcp)) {
                pr_warn(STR("Connection slots full. Rejecting.\n"));
                tcp_conn_close(&tcp, setup);
                break;
            }
            progress = true;
        }

        /* Step all active connections. */
        for (sz i = 0; i < WEB_MAX_CONNS; i++) {
            if (srv.conns[i].tcp) {
                progress |= web_step_conn(&srv, &srv.conns[i]);
                sched_note_activity();
            }
        }

        /* Timeout idle connections waiting for a new request (keep-alive). */
        u64 now = time_current_ms().ms;
        for (sz i = 0; i < WEB_MAX_CONNS; i++) {
            struct web_conn *wc = &srv.conns[i];
            if (wc->tcp && wc->state == WCS_RECV &&
                now - wc->last_activity_ms > WEB_KEEPALIVE_TIMEOUT_MS)
                web_conn_free(wc);
        }

        sleep_ms(time_ms_new(progress ? 0 : 10));
    }
}
