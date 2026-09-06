#define _GNU_SOURCE

#include "hashtable_mem.h"

#include <stdlib.h>
#include <sys/mman.h>

#define HT_HUGE ((size_t)2 << 20)

void *ht_alloc_pages(size_t bytes, size_t *bytes_out)
{
    bytes = (bytes + HT_HUGE - 1) & ~(HT_HUGE - 1);

    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;

    (void)madvise(p, bytes, MADV_HUGEPAGE);

    *bytes_out = bytes;
    return p;
}

void ht_free_pages(void *p, size_t bytes)
{
    munmap(p, bytes);
}

void ht_populate(void *p, size_t off, size_t ahead, size_t total)
{
    size_t start = off & ~(HT_HUGE - 1);
    if (start >= total)
        return;

    size_t len = ahead;
    if (len > total - start)
        len = total - start;

    madvise((char *)p + start, len, MADV_POPULATE_WRITE);
}