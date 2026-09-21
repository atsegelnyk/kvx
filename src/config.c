#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "flag.h"

static bool parse_port(const char *port)
{
    if (*port == '\0')
        return false;

    errno = 0;

    char *end;
    unsigned long value = strtoul(port, &end, 10);

    if (errno != 0)
        return false;

    if (*end != '\0')
        return false;

    if (value == 0 || value > UINT16_MAX)
        return false;

    return true;
}

static bool parse_listener(char *value, listener_config_t *listener)
{
    static const char tcp_prefix[] = "tcp://";
    static const char unix_prefix[] = "unix://";
    if (strncmp(value, tcp_prefix, sizeof(tcp_prefix) - 1) == 0)
    {
        char *addr = value + sizeof(tcp_prefix) - 1;

        char *colon = strchr(addr, ':');
        if (colon == NULL)
            return false;

        if (strchr(colon + 1, ':') != NULL)
            return false;

        char *port = colon + 1;

        if (!parse_port(port))
            return false;

        *colon = '\0';

        listener->type = LISTENER_TCP;
        listener->host = *addr != '\0' ? addr : NULL;
        listener->port = port;

        return true;
    }

    if (strncmp(value, unix_prefix, sizeof(unix_prefix) - 1) == 0)
    {
        char *path = value + sizeof(unix_prefix) - 1;

        if (*path == '\0')
            return false;

        listener->type = LISTENER_UNIX;
        listener->unix_path = path;

        return true;
    }

    return false;
}

bool parse_config(int argc, char **argv, config_t *conifg)
{
    const char *listen = NULL;
    flag_str(
        "l",
        "listen",
        &listen,
        "for tcp use addr:port scheme: tcp://:1234\n"
        "\t\tfor uds use path to socket: unix:///var/run/kvx.sock");

    if (flag_parse(argc, argv) != FLAG_OK)
        return false;

    if (listen == NULL)
        return false;

    listener_config_t listener = {0};
    if (!parse_listener((char *)listen, &listener))
        return false;

    conifg->listener = listener;

    return true;
}