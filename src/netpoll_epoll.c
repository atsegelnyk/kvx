#include "netpoll.h"

#ifdef NETPOLL_EPOLL

#include "unistd.h"
#include <stddef.h>
#include <stdlib.h>
#include <sys/epoll.h>

struct netpoll_t
{
    int fd;

    struct epoll_event *events;
    size_t events_cap;
};

netpoll_t *netpoll_init(size_t events_cap)
{
    netpoll_t *poll = malloc(sizeof(*poll));
    if (!poll)
        return NULL;

    struct epoll_event *events = malloc(sizeof(struct epoll_event) * events_cap);
    if (!poll->events)
    {
        free(poll);
        return NULL;
    }

    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0)
    {
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
    free(poll);
}

int netpoll_add(netpoll_t *poll, int fd, uint32_t data)
{
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLRDHUP,
        .data.u32 = data,
    };

    return epoll_ctl(poll->fd, EPOLL_CTL_ADD, fd, &ev);
}

int netpoll_mod_r(netpoll_t *poll, int fd, uint32_t data)
{
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLRDHUP,
        .data.u32 = data,
    };

    return epoll_ctl(poll->fd, EPOLL_CTL_MOD, fd, &ev);
}

int netpoll_mod_rw(netpoll_t *poll, int fd, uint32_t data)
{
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP,
        .data.u32 = data,
    };

    return epoll_ctl(poll->fd, EPOLL_CTL_MOD, fd, &ev);
}

int netpoll_del(netpoll_t *poll, int fd)
{
    return epoll_ctl(poll->fd, EPOLL_CTL_DEL, fd, NULL);
}

int netpoll_poll(netpoll_t *poll, netpoll_event_t **events, int timeout_ms)
{
    int n = epoll_wait(poll->fd, poll->events, (int)poll->events_cap, timeout_ms);
    if (n < 0)
        return -1;

    *events = poll->events;

    return n;
}

#endif
