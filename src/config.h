#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef enum listener_type_t
{
    LISTENER_TCP,
    LISTENER_UNIX,
} listener_type_t;

typedef struct listener_config_t
{
    listener_type_t type;

    const char *host;
    const char *port;

    const char *unix_path;
} listener_config_t;

typedef struct config_t
{
    listener_config_t listener;
} config_t;

bool parse_config(int argc, char **argv, config_t *conifg);

#endif // CONFIG_H