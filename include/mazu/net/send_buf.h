/* SPDX-License-Identifier: MIT */
/* Efficient buffer for constructing packets to be sent over the network.
 *
 * Protocol layers prepend headers as a packet moves down the stack. send_buf
 * lets each layer write its own header once, then assemble the final payload
 * in reverse part order at the end.
 *
 * Flow:
 *
 *     payload
 *       |
 *       v
 *   send_buf_prepend(TCP header)
 *       |
 *       v
 *   send_buf_prepend(IP header)
 *       |
 *       v
 *   send_buf_prepend(Ethernet header)
 *       |
 *       v
 *   send_buf_assemble() -> [Ethernet][IP][TCP][payload]
 */

#ifndef MAZU_NET_SEND_BUF_H
#define MAZU_NET_SEND_BUF_H

#include <mazu/arena.h>
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/error.h>

#define SEND_BUF_NUM_PARTS 8

struct send_buf {
    struct arena orig_arn; /* copy to allow resetting */
    struct arena arn;
    struct byte_buf parts[SEND_BUF_NUM_PARTS];
    sz n_used;
};

/* Create a new send buffer that uses 'arn' for its underlying memory. */
struct send_buf send_buf_new(struct arena arn);

/* Get a new byte buffer from the send buffer. The bytes written to the new
 * buffer will be prepended to the content of all existing buffers in the send
 * buffer when assembling the complete content of the send buffer.
 */
struct byte_buf *send_buf_prepend(struct send_buf *sb, sz buf_size);

/* Compute the total length of the send buffer's content. I.e., the length of
 * the content appended to the byte buffer when calling send_buf_assemble().
 */
sz send_buf_total_length(struct send_buf sb);

/* Reset the buffer to be completely empty. */
void send_buf_clear(struct send_buf *sb);

/* Append the complete content of the send buffer to 'buf'. */
struct result send_buf_assemble(struct send_buf sb, struct byte_buf *buf);

#endif /* MAZU_NET_SEND_BUF_H */
