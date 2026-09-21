#ifndef NETPOOL_H
#define NETPOOL_H

#include "stddef.h"
#include <stdbool.h>
#include <stdint.h>

#if defined(__linux__)

#include <sys/epoll.h>

#define NETPOLL_EPOLL 1

#elif defined(__APPLE__) || \
    defined(__FreeBSD__) || \
    defined(__NetBSD__) ||  \
    defined(__OpenBSD__) || \
    defined(__DragonFly__)

#include <sys/event.h>

#define NETPOLL_KQUEUE 1

#else

#error "unsupported netpoll backend"

#endif

typedef struct netpoll_t netpoll_t;

#if defined(NETPOLL_EPOLL)

typedef struct epoll_event netpoll_event_t;

static inline uint32_t netpoll_event_data(const netpoll_event_t *event)
{
    return event->data.u32;
}

static inline bool netpoll_event_readable(const netpoll_event_t *event)
{
    return event->events &
           (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
}

static inline bool netpoll_event_writable(const netpoll_event_t *event)
{
    return event->events &
           (EPOLLOUT | EPOLLERR | EPOLLHUP);
}

#elif defined(NETPOLL_KQUEUE)

typedef struct kevent netpoll_event_t;

static inline uint32_t netpoll_event_data(const netpoll_event_t *event)
{
    return (uint32_t)(uintptr_t)event->udata;
}

static inline bool netpoll_event_readable(const netpoll_event_t *event)
{
    return event->filter == EVFILT_READ;
}

static inline bool netpoll_event_writable(const netpoll_event_t *event)
{
    return event->filter == EVFILT_WRITE;
}

#endif

#define NETPOLL_READABLE (1u << 0)
#define NETPOLL_WRITABLE (1u << 1)

netpoll_t *netpoll_init(size_t events_cap);
void netpoll_destroy(netpoll_t *poll);

int netpoll_add(netpoll_t *poll, int fd, uint32_t data);
int netpoll_mod_r(netpoll_t *poll, int fd, uint32_t data);
int netpoll_mod_rw(netpoll_t *poll, int fd, uint32_t data);
int netpoll_del(netpoll_t *poll, int fd);
int netpoll_poll(netpoll_t *poll, netpoll_event_t **events, int timeout_ms);

#endif // NETPOOL_H
