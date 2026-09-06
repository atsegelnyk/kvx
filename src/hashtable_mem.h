#ifndef HASHTABLE_MEM_H
#define HASHTABLE_MEM_H

#include <stddef.h>

void *ht_alloc_pages(size_t bytes, size_t *bytes_out);
void ht_free_pages(void *p, size_t bytes);
void ht_populate(void *p, size_t off, size_t ahead, size_t total);

#endif // HASHTABLE_MEM_H