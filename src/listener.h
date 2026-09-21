#ifndef KVX_LISTENER_H
#define KVX_LISTENER_H

#include <stdbool.h>

bool tcp_listener_per_worker(void);

/* host: IPv4 address, unbracketed IPv6 address, or hostname.
 * NULL or empty host binds the IPv4 wildcard (0.0.0.0).
 * port: non-NULL, nonempty decimal string in 0..65535; 0 selects a free port.
 * Always enables SO_REUSEADDR. Enables TCP load-balanced port reuse only on
 * platforms selected by listener_tcp_per_worker(). IPv6 sockets are IPv6-only.
 * Socket-option failures return -1; they do not silently change worker mode.
 * Returns one listener, using the first address that binds/listens.
 * DNS failures map to ENOMEM, EAGAIN, or EADDRNOTAVAIL; EAI_SYSTEM keeps errno.
 */
int listener_create_tcp(const char *host, const char *port);

/* path: non-NULL, nonempty, NUL-terminated filesystem pathname.
 * Caller MUST ensure strlen(path) < sizeof(((struct sockaddr_un *)0)->sun_path)
 * on the target platform (struct sockaddr_un is declared in <sys/un.h>).
 * Existing paths are not removed. Caller owns unlink after successful use.
 * Create once and share across workers; one pathname cannot have independent
 * listeners. Permissions follow the process umask.
 */
int listener_create_unix(const char *path);

#endif
