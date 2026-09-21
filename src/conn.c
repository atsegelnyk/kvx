#define _POSIX_C_SOURCE 200809L

#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(MSG_NOSIGNAL)
#define CONN_SEND_FLAGS MSG_NOSIGNAL
#elif defined(SO_NOSIGPIPE)
#define CONN_SEND_FLAGS 0
#else
#error "This platform needs a SIGPIPE suppression implementation"
#endif

static size_t max_buffer_size;

static bool buffer_grow(uint8_t **buf, size_t *cap, size_t extra)
{
    if (extra > max_buffer_size - *cap)
    {
        errno = ENOBUFS;
        return false;
    }

    if (extra == 0)
        return true;

    size_t min_cap = *cap + extra;
    size_t new_cap = *cap ? *cap : CONN_DEFAULT_BUF_CAP;

    while (new_cap < min_cap)
    {
        if (new_cap > max_buffer_size / 2)
        {
            new_cap = max_buffer_size;
            break;
        }

        new_cap *= 2;
    }

    uint8_t *new_buf = realloc(*buf, new_cap);
    if (new_buf == NULL)
        return false;

    *buf = new_buf;
    *cap = new_cap;

    return true;
}

static void buffer_compact(uint8_t *buf, size_t *len, size_t *pos)
{
    if (*pos == 0)
        return;

    size_t remaining = *len - *pos;
    if (remaining != 0)
        memmove(buf, buf + *pos, remaining);

    *len = remaining;
    *pos = 0;
}

bool conn_set_max_buffer_size(size_t size)
{
    if (max_buffer_size != 0)
    {
        errno = EALREADY;
        return false;
    }

    if (size < CONN_DEFAULT_BUF_CAP)
    {
        errno = EINVAL;
        return false;
    }

    max_buffer_size = size;
    return true;
}

bool conn_init(conn_t *conn)
{
    *conn = (conn_t){.fd = -1};

    if (max_buffer_size == 0)
    {
        errno = EINVAL;
        return false;
    }

    if (!buffer_grow(&conn->r_buf, &conn->r_buf_cap, CONN_DEFAULT_BUF_CAP) ||
        !buffer_grow(&conn->w_buf, &conn->w_buf_cap, CONN_DEFAULT_BUF_CAP))
    {
        int saved_errno = errno;
        conn_destroy(conn);
        errno = saved_errno;
        return false;
    }

    return true;
}

bool conn_attach(conn_t *conn, int fd)
{
    if (fd < 0 || conn->fd != -1 ||
        conn->r_buf == NULL || conn->w_buf == NULL)
    {
        errno = EINVAL;
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return false;

#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &enabled, sizeof(enabled)) == -1)
        return false;
#endif

    conn->fd = fd;
    conn->peer_closed = false;
    conn->r_len = conn->r_pos = 0;
    conn->w_len = conn->w_pos = 0;
    return true;
}

void conn_reset(conn_t *conn)
{
    if (conn->fd >= 0)
        close(conn->fd);

    conn->fd = -1;
    conn->peer_closed = false;
    conn->r_len = 0;
    conn->r_pos = 0;
    conn->w_len = 0;
    conn->w_pos = 0;
}

void conn_destroy(conn_t *conn)
{
    conn_reset(conn);
    free(conn->r_buf);
    free(conn->w_buf);
    *conn = (conn_t){.fd = -1};
}

conn_io_result_t conn_recv(conn_t *conn)
{
    if (conn->fd < 0)
    {
        errno = EBADF;
        return CONN_IO_ERROR;
    }

    if (conn->peer_closed)
        return CONN_IO_EOF;

    for (;;)
    {
        if (conn->r_len == conn->r_buf_cap)
        {
            buffer_compact(conn->r_buf, &conn->r_len, &conn->r_pos);

            if (conn->r_len == conn->r_buf_cap)
            {
                if (conn->r_buf_cap == max_buffer_size)
                    return CONN_IO_FULL;

                if (!buffer_grow(&conn->r_buf, &conn->r_buf_cap, 1))
                    return CONN_IO_ERROR;
            }
        }

        size_t available = conn->r_buf_cap - conn->r_len;

        if (available > (size_t)SSIZE_MAX)
            available = (size_t)SSIZE_MAX;

        ssize_t n = recv(conn->fd, conn->r_buf + conn->r_len, available, 0);

        if (n > 0)
        {
            conn->r_len += (size_t)n;
            continue;
        }

        if (n == 0)
        {
            conn->peer_closed = true;
            return CONN_IO_EOF;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return CONN_IO_AGAIN;

        return CONN_IO_ERROR;
    }
}

bool conn_consume(conn_t *conn, size_t count)
{
    if (count > conn->r_len - conn->r_pos)
    {
        errno = EINVAL;
        return false;
    }

    conn->r_pos += count;
    if (conn->r_pos == conn->r_len)
        conn->r_pos = conn->r_len = 0;

    return true;
}

bool conn_write(conn_t *conn, const uint8_t *data, size_t len)
{
    if (len == 0)
        return true;

    if (data == NULL)
    {
        errno = EINVAL;
        return false;
    }

    size_t pending = conn->w_len - conn->w_pos;
    if (len > max_buffer_size - pending)
    {
        errno = ENOBUFS;
        return false;
    }

    if (len > conn->w_buf_cap - conn->w_len)
    {
        buffer_compact(conn->w_buf, &conn->w_len, &conn->w_pos);
        size_t available = conn->w_buf_cap - conn->w_len;
        if (len > available)
        {
            if (!buffer_grow(&conn->w_buf, &conn->w_buf_cap, len - available))
                return false;
        }
    }

    memcpy(conn->w_buf + conn->w_len, data, len);
    conn->w_len += len;

    return true;
}

conn_io_result_t conn_flush(conn_t *conn)
{
    if (conn->fd < 0)
    {
        errno = EBADF;
        return CONN_IO_ERROR;
    }

    while (conn_wants_pollout(conn))
    {
        size_t remaining = conn->w_len - conn->w_pos;
        if (remaining > (size_t)SSIZE_MAX)
            remaining = (size_t)SSIZE_MAX;

        ssize_t n = send(conn->fd, conn->w_buf + conn->w_pos,
                         remaining, CONN_SEND_FLAGS);

        if (n > 0)
        {
            conn->w_pos += (size_t)n;
            continue;
        }

        if (n == 0)
        {
            errno = EIO; /* Avoid spinning if send makes no progress. */
            return CONN_IO_ERROR;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return CONN_IO_AGAIN;

        return CONN_IO_ERROR;
    }

    conn->w_pos = conn->w_len = 0;

    return CONN_IO_OK;
}
