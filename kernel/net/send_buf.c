/* SPDX-License-Identifier: MIT */
#include <mazu/net/send_buf.h>

/* send_buf stores prepended pieces in part order and assembles them in reverse.
 * Each part comes from the buffer's arena, so arena capacity bounds the total
 * payload size.
 */

struct send_buf send_buf_new(struct arena arn)
{
    struct send_buf sb = {
        .orig_arn = arn,
        .arn = arn,
        .n_used = 0,
    };
    byte_array_set(byte_array_new(&sb.parts, sizeof(sb.parts)), 0);
    return sb;
}

struct byte_buf *send_buf_prepend(struct send_buf *sb, sz buf_size)
{
    assert(sb);

    if (sb->n_used == SEND_BUF_NUM_PARTS)
        return NULL;

    struct byte_buf *buf = &sb->parts[sb->n_used];
    *buf = byte_buf_from_array(byte_array_from_arena(buf_size, &sb->arn));
    sb->n_used++;

    return buf;
}

sz send_buf_total_length(struct send_buf sb)
{
    sz len = 0;

    for (sz i = 0; i < sb.n_used; i++)
        len += sb.parts[i].len;

    return len;
}

void send_buf_clear(struct send_buf *sb)
{
    sb->arn = sb->orig_arn;
    sb->n_used = 0;
}

struct result send_buf_assemble(struct send_buf sb, struct byte_buf *buf)
{
    assert(buf);

    sz len_before = buf->len;

    /* Data is prepended to a send buffer by appending it to the buffer
     * 'sb.parts[sb.n_used]' and then incrementing the 'n_used' member so the
     * next prepend operation uses the next buffer. Hence, to assemble the
     * content of the send buffer in the right order, append the content of all
     * parts to 'buf' starting with 'sb.parts[sb.n_used - 1]' and working
     * backwards.
     */

    for (sz i = sb.n_used; i > 0; i--) {
        sz n_appended =
            byte_buf_append(buf, byte_view_from_buf(sb.parts[i - 1]));
        if (n_appended != sb.parts[i - 1].len)
            return result_error(ENOMEM);
    }

    /* Verify correctness. Callers rely on this invariant holding true. */
    assert(buf->len - len_before == send_buf_total_length(sb));

    return result_ok();
}
