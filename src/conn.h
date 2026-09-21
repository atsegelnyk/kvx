#ifndef KVX_CONN_H
#define KVX_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONN_DEFAULT_BUF_CAP ((size_t)4096)

typedef struct conn_t
{
    int fd;
    bool peer_closed;

    uint8_t *r_buf;
    size_t r_buf_cap;
    size_t r_len;
    size_t r_pos;

    uint8_t *w_buf;
    size_t w_buf_cap;
    size_t w_len;
    size_t w_pos;
} conn_t;

typedef enum conn_io_result_t
{
    CONN_IO_ERROR = -1, /* errno describes the failure. */
    CONN_IO_OK,         /* Flush: all queued output sent. */
    CONN_IO_AGAIN,      /* recv/send reached EAGAIN or EWOULDBLOCK. */
    CONN_IO_EOF,        /* Receive: peer shut down its sending direction. */
    CONN_IO_FULL,       /* Receive: buffer at its limit; kernel may have more. */
} conn_io_result_t;

/* Set the shared read/write limit once during single-threaded startup, before
 * starting workers or initializing connections. The limit must be >= 4096.
 * Returns false with EINVAL for an invalid limit, or EALREADY if already set.
 * A failed call does not change the setting. No runtime reconfiguration.
 */
bool conn_set_max_buffer_size(size_t size);

/* Single-worker ownership. Initialize once, including before reset/destroy.
 * Requires conn_set_max_buffer_size() first; otherwise fails with EINVAL.
 * On failure the object is empty and safe to destroy. Do not init a live conn.
 */
bool conn_init(conn_t *conn);

/* Attach to an initialized, reset conn. Sets O_NONBLOCK and suppresses SIGPIPE.
 * Ownership of fd transfers only on success; failure leaves fd with caller.
 * Register fd with the worker's poller after successful attachment.
 */
bool conn_attach(conn_t *conn, int fd);

/* Unregister from the poller before reset/destroy. Reset closes fd, drops all
 * buffered data, and retains capacity for pooling. Destroy also frees buffers.
 * The pool must prevent stale poll events from accessing a reused object.
 */
void conn_reset(conn_t *conn);
void conn_destroy(conn_t *conn);

/* Read until AGAIN, EOF, FULL, or ERROR. Compacts and grows automatically up
 * to the configured maximum (includes command headers and framing).
 * Every result can follow successful reads: inspect r_buf + r_pos and
 * r_len - r_pos even on AGAIN/EOF. On ERROR the worker should close the conn.
 * FULL requires parsing/consuming, then calling recv again. If an incomplete
 * command occupies the entire limit, the worker must reject that command.
 * With edge-triggered polling do not wait for another edge after FULL.
 * Finish using parsed pointers before another receive, compaction, or growth.
 */
conn_io_result_t conn_recv(conn_t *conn);

/* Consume count bytes from the unread span. O(1); no immediate memmove.
 * Resets both offsets when empty. Receive compacts later when it needs room.
 * Returns false with EINVAL if count exceeds unread bytes.
 */
bool conn_consume(conn_t *conn, size_t count);

/* Append a copy to the output queue, compacting/growing as needed.
 * Pending bytes plus len must fit the configured maximum; otherwise returns
 * false with ENOBUFS, without appending anything. Allocation failure: ENOMEM.
 * data must not point into w_buf (which can move). No socket I/O here.
 * On failure queued bytes remain intact, though their addresses may change.
 */
bool conn_write(conn_t *conn, const uint8_t *data, size_t len);

/* Send until drained, AGAIN, or ERROR. Handles partial writes.
 * Call immediately after queuing output, then request writable events only
 * while conn_wants_pollout() is true. ERROR requires closing the conn.
 */
conn_io_result_t conn_flush(conn_t *conn);

static inline bool conn_wants_pollout(const conn_t *conn)
{
    return conn->w_pos < conn->w_len;
}

#endif
