#ifndef PAGES_H
#define PAGES_H

#include <stddef.h>

#define PAGES_HUGE ((size_t)2 << 20)

void *pages_map(size_t bytes, size_t *bytes_out);
void pages_unmap(void *p, size_t bytes);
void pages_populate(void *p, size_t off, size_t ahead, size_t total);

#endif // PAGES_H
