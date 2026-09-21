/* Expose socket extensions in glibc/macOS headers under strict C builds. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include "listener.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LISTENER_BACKLOG 1024

#if defined(__FreeBSD__) && defined(SO_REUSEPORT_LB)
#define LISTENER_TCP_REUSEPORT_OPTION SO_REUSEPORT_LB
#elif (defined(__linux__) || defined(__DragonFly__)) && defined(SO_REUSEPORT)
#define LISTENER_TCP_REUSEPORT_OPTION SO_REUSEPORT
#endif

bool tcp_listener_per_worker(void)
{
#ifdef LISTENER_TCP_REUSEPORT_OPTION
    return true;
#else
    return false;
#endif
}

static int close_failed_socket(int fd)
{
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

static int create_socket(int family, int protocol)
{
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    return socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#else
    int fd = socket(family, SOCK_STREAM, protocol);
    if (fd == -1)
        return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return close_failed_socket(fd);

    flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
        return close_failed_socket(fd);

    return fd;
#endif
}

static int enable_tcp_reuse(int fd)
{
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) == -1)
        return -1;

#ifdef LISTENER_TCP_REUSEPORT_OPTION
    return setsockopt(fd, SOL_SOCKET, LISTENER_TCP_REUSEPORT_OPTION,
                      &enabled, sizeof(enabled));
#else
    return 0;
#endif
}

int listener_create_tcp(const char *host, const char *port)
{
    if (host == NULL || *host == '\0')
        host = "0.0.0.0";

    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE | AI_NUMERICSERV,
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };

    struct addrinfo *addresses = NULL;
    int error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0)
    {
        if (error == EAI_MEMORY)
            errno = ENOMEM;
        else if (error == EAI_AGAIN)
            errno = EAGAIN;
        else if (error != EAI_SYSTEM)
            errno = EADDRNOTAVAIL;

        return -1;
    }

    int last_errno = EADDRNOTAVAIL;
    int listener_fd = -1;

    for (struct addrinfo *addr = addresses; addr != NULL; addr = addr->ai_next)
    {
        int fd = create_socket(addr->ai_family, addr->ai_protocol);
        if (fd == -1)
        {
            last_errno = errno;
            continue;
        }

        if (enable_tcp_reuse(fd) == -1)
            goto next_address;

        if (addr->ai_family == AF_INET6)
        {
            int enabled = 1;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                           &enabled, sizeof(enabled)) == -1)
                goto next_address;
        }

        if (bind(fd, addr->ai_addr, addr->ai_addrlen) == -1)
            goto next_address;

        if (listen(fd, LISTENER_BACKLOG) == -1)
            goto next_address;

        listener_fd = fd;
        break;

    next_address:
        last_errno = errno;
        close(fd);
    }

    freeaddrinfo(addresses);
    if (listener_fd == -1)
        errno = last_errno;

    return listener_fd;
}

int listener_create_unix(const char *path)
{
    struct sockaddr_un addr = {0};
    size_t path_len = strlen(path);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    addr.sun_len = (unsigned char)addr_len;
#endif

    int fd = create_socket(AF_UNIX, 0);
    if (fd == -1)
        return -1;

    if (bind(fd, (struct sockaddr *)&addr, addr_len) == -1)
        return close_failed_socket(fd);

    if (listen(fd, LISTENER_BACKLOG) == -1)
    {
        int saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        return -1;
    }

    return fd;
}
