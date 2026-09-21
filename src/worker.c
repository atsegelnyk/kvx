#include "worker.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#include "conn.h"
#include "netpoll.h"

typedef struct acceptor_worker_t
{
    pthread_t thread;

    int listener_fd;
    netpoll_t *poller;

    worker_t **workers;
    size_t workers_num;
    size_t next_worker;
} acceptor_worker_t;

struct worker_t
{
    size_t id;
    pthread_t thread;

    int listener_fd; // acceptor mode
    netpoll_t *poller;

    worker_mode_t mode;

    conn_t *conns;
    size_t conns_num;               // acceptor mode
    atomic_size_t atomic_conns_num; // dispatcher mode
    size_t conns_cap;
};