#include "netpoll.h"

#ifdef NETPOLL_KQUEUE

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

struct netpoll_t
{
    int fd;

    struct kevent *events;
    size_t events_cap;
};

static int netpoll_update(netpoll_t *poll, int fd, uint32_t data, int write_enabled)
{
    struct kevent changes[2];

    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void *)(uintptr_t)data);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | (write_enabled ? EV_ENABLE : EV_DISABLE), 0, 0, (void *)(uintptr_t)data);

    return kevent(poll->fd, changes, 2, NULL, 0, NULL);
}

netpoll_t *netpoll_init(size_t events_cap)
{
    netpoll_t *poll = malloc(sizeof(*poll));
    if (!poll)
        return NULL;

    struct kevent *events = malloc(sizeof(struct kevent) * events_cap);
    if (!events)
    {
        free(poll);
        return NULL;
    }

    int fd = kqueue();
    if (fd < 0)
    {
        free(events);
        free(poll);
        return NULL;
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    {
        close(fd);
        free(events);
        free(poll);
        return NULL;
    }

    poll->fd = fd;
    poll->events = events;
    poll->events_cap = events_cap;

    return poll;
}

void netpoll_destroy(netpoll_t *poll)
{
    close(poll->fd);
    free(poll->events);
    free(poll);
}

int netpoll_add(netpoll_t *poll, int fd, uint32_t data)
{
    return netpoll_update(poll, fd, data, 0);
}

int netpoll_mod_r(netpoll_t *poll, int fd, uint32_t data)
{
    return netpoll_update(poll, fd, data, 0);
}

int netpoll_mod_rw(netpoll_t *poll, int fd, uint32_t data)
{
    return netpoll_update(poll, fd, data, 1);
}

int netpoll_del(netpoll_t *poll, int fd)
{
    struct kevent changes[2];

    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    return kevent(poll->fd, changes, 2, NULL, 0, NULL);
}

int netpoll_poll(netpoll_t *poll, netpoll_event_t **events, int timeout_ms)
{
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;

    if (timeout_ms >= 0)
    {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;
        timeout_ptr = &timeout;
    }

    int n = kevent(poll->fd, NULL, 0, poll->events, (int)poll->events_cap, timeout_ptr);
    if (n < 0)
        return -1;

    *events = poll->events;

    return n;
}

#endif
