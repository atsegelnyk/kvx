#ifndef WORKER_H
#define WORKER_H

typedef enum worker_mode_t
{
    WORKER_MODE_ACCEPT,
    WORKER_MODE_DISPATCH,
} worker_mode_t;

typedef struct worker_t worker_t;

#endif // WORKER_H