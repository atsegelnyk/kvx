#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stddef.h>

#include "item.h"

typedef struct slab_allocator_t slab_allocator_t;

slab_allocator_t *slab_allocator_init(void);
void slab_allocator_destroy(slab_allocator_t *alloc);

size_t slab_allocator_usable_size(slab_allocator_t *alloc, size_t size);

item_t *slab_allocator_alloc_item(slab_allocator_t *alloc, size_t size);
void slab_allocator_free_item(item_t *item);

#endif // SLAB_ALLOC_H
