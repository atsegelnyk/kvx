#define _GNU_SOURCE

#include "pages.h"

#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#if !defined(__unix__) && !defined(__APPLE__)
#error "pages.c requires a POSIX host"
#endif

#include <sys/mman.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

static size_t page_size(void)
{
    static size_t cached;
    if (cached == 0)
        cached = (size_t)sysconf(_SC_PAGESIZE);

    return cached;
}

static size_t align_up(size_t v, size_t align)
{
    return (v + align - 1) & ~(align - 1);
}

void *pages_map(size_t bytes, size_t *bytes_out)
{
    int huge = (bytes >= PAGES_HUGE);

    bytes = align_up(bytes, huge ? PAGES_HUGE : page_size());

    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;

    if (huge)
    {
#ifdef MADV_HUGEPAGE
        (void)madvise(p, bytes, MADV_HUGEPAGE);
#endif
    }

    if (bytes_out)
        *bytes_out = bytes;

    return p;
}

void pages_unmap(void *p, size_t bytes)
{
    munmap(p, bytes);
}

void pages_populate(void *p, size_t off, size_t ahead, size_t total)
{
    size_t start = off & ~(PAGES_HUGE - 1);
    if (start >= total)
        return;

    size_t len = ahead;
    if (len > total - start)
        len = total - start;

#if defined(MADV_POPULATE_WRITE)
    (void)madvise((char *)p + start, len, MADV_POPULATE_WRITE);
#else
    volatile unsigned char *q = (volatile unsigned char *)p + start;
    size_t step = page_size();
    for (size_t i = 0; i < len; i += step)
        q[i] = q[i];
#endif
}
