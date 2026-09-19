#include "slab_alloc.h"

#include <inttypes.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "item.h"
#include "pages.h"

#define SLAB_SIZE (1UL << 21)

#define SLAB_CLASS_INITIAL_CAP 8

#define MIN_DATA_CHUNK_SIZE 48
#define MAX_DATA_CHUNK_SIZE (1UL << 19)

#define CHUNK_GROWTH_FACTOR 1.25

static inline size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

typedef struct slab_t slab_t;

typedef struct slab_class_t slab_class_t;
static void slab_class_free_item(slab_class_t *sc, slab_t *slab);

struct slab_t
{
    size_t chunk_size;

    void *next_chunk;
    size_t free_count;

    slab_class_t *parent_slab_class;

    uint8_t mem[];
};

static slab_t *slab_init(size_t data_chunk_size, slab_class_t *parent_slab_class)
{
    size_t chunk_size = align_up(data_chunk_size, alignof(item_t));

    size_t free_count = (SLAB_SIZE - sizeof(slab_t)) / chunk_size;

    slab_t *slab = pages_map(SLAB_SIZE, NULL);
    if (!slab)
        abort();

    slab->chunk_size = chunk_size;
    slab->parent_slab_class = parent_slab_class;
    slab->next_chunk = &slab->mem[0];
    slab->free_count = free_count;

    for (size_t i = 0; i < free_count; i++)
    {
        void *current = &slab->mem[i * chunk_size];
        void *next = NULL;
        if (i + 1 < free_count)
            next = &slab->mem[(i + 1) * chunk_size];

        memcpy(current, &next, sizeof(next));
    }

    return slab;
}

static void slab_destroy(slab_t *slab)
{
    pages_unmap(slab, SLAB_SIZE);
}

static item_t *slab_alloc_item(slab_t *slab)
{
    if (slab->free_count == 0)
        return NULL;

    void *current = slab->next_chunk;
    void *next;

    item_t *item = (item_t *)(current);
    memcpy(&next, current, sizeof(next));

    item->slab_ptr = slab;
    item->data_size = slab->chunk_size;

    slab->next_chunk = next;
    slab->free_count--;

    return item;
}

void slab_allocator_free_item(item_t *item)
{
    slab_t *slab = item->slab_ptr;

    memcpy(item, &slab->next_chunk, sizeof(slab->next_chunk));

    slab->free_count++;
    slab->next_chunk = item;

    slab_class_free_item(slab->parent_slab_class, slab);
}

struct slab_class_t
{
    size_t data_chunk_size;

    size_t free_top;
    slab_t **free_stack;

    size_t slabs_num;
    size_t slabs_cap;
    slab_t **slabs;
};

static void slab_class_init(slab_class_t *sc, size_t chunk_size)
{
    slab_t **slabs = malloc(sizeof(slab_t *) * SLAB_CLASS_INITIAL_CAP);
    if (!slabs)
        abort();

    slab_t **free_stack = malloc(sizeof(slab_t *) * SLAB_CLASS_INITIAL_CAP);
    if (!free_stack)
        abort();

    sc->data_chunk_size = chunk_size;

    sc->free_top = 0;
    sc->free_stack = free_stack;

    sc->slabs_num = 0;
    sc->slabs_cap = SLAB_CLASS_INITIAL_CAP;
    sc->slabs = slabs;
}

static void slab_class_destroy(slab_class_t *sc)
{
    for (size_t i = 0; i < sc->slabs_num; i++)
    {
        slab_destroy(sc->slabs[i]);
    }

    free(sc->free_stack);
    free(sc->slabs);
}

static void slab_class_grow_slabs(slab_class_t *sc)
{
    size_t new_cap = sc->slabs_cap + sc->slabs_cap / 2;

    sc->slabs = realloc(sc->slabs, sizeof(slab_t *) * new_cap);
    if (!sc->slabs)
        abort();

    sc->free_stack = realloc(sc->free_stack, sizeof(slab_t *) * new_cap);
    if (!sc->free_stack)
        abort();

    sc->slabs_cap = new_cap;
}

static void slab_class_grow(slab_class_t *sc)
{
    if (sc->slabs_num == sc->slabs_cap)
    {
        slab_class_grow_slabs(sc);
    }

    slab_t *new_slab = slab_init(sc->data_chunk_size, sc);

    sc->slabs[sc->slabs_num++] = new_slab;
    sc->free_stack[sc->free_top++] = new_slab;
}

static void *slab_class_alloc_item(slab_class_t *sc)
{
    if (sc->free_top == 0)
    {
        slab_class_grow(sc);
    }

    slab_t *slab = sc->free_stack[sc->free_top - 1];

    item_t *item = slab_alloc_item(slab);
    if (slab->free_count == 0)
    {
        sc->free_top--;
    }

    return item;
}

static void slab_class_free_item(slab_class_t *sc, slab_t *slab)
{
    if (slab->free_count == 1)
    {
        sc->free_stack[sc->free_top++] = slab;
    }
}

struct slab_allocator_t
{
    size_t classes_num;
    slab_class_t *classes;
};

static inline size_t next_chunk_size(size_t size)
{
    size_t next =
        align_up((size_t)(size * CHUNK_GROWTH_FACTOR), alignof(item_t));

    if (next <= size)
        next = size + alignof(item_t);

    if (next > MAX_DATA_CHUNK_SIZE)
        next = MAX_DATA_CHUNK_SIZE;

    return next;
}

static size_t count_slab_classes(void)
{
    size_t size = MIN_DATA_CHUNK_SIZE;
    size_t count = 1;

    while (size < MAX_DATA_CHUNK_SIZE)
    {
        size = next_chunk_size(size);
        count++;
    }

    return count;
}

slab_allocator_t *slab_allocator_init(void)
{
    size_t num_slab_classes = count_slab_classes();

    slab_allocator_t *alloc = malloc(sizeof(slab_allocator_t));
    if (!alloc)
        abort();

    slab_class_t *classes = malloc(sizeof(slab_class_t) * num_slab_classes);
    if (!classes)
        abort();

    alloc->classes = classes;
    alloc->classes_num = num_slab_classes;

    size_t chunk_size = MIN_DATA_CHUNK_SIZE;
    for (size_t i = 0; i < num_slab_classes; i++)
    {
        slab_class_init(&alloc->classes[i], chunk_size);
        chunk_size = next_chunk_size(chunk_size);
    }

    return alloc;
}

void slab_allocator_destroy(slab_allocator_t *alloc)
{
    for (size_t i = 0; i < alloc->classes_num; i++)
    {
        slab_class_destroy(&alloc->classes[i]);
    }

    free(alloc->classes);
    free(alloc);
}

static inline size_t slab_allocator_find_class(slab_allocator_t *alloc, size_t size)
{
    size_t left = 0;
    size_t right = alloc->classes_num;

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;

        if (alloc->classes[mid].data_chunk_size < size)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    return left;
}

item_t *slab_allocator_alloc_item(slab_allocator_t *alloc, size_t data_len)
{
    size_t size = data_len + ITEM_SIZE;

    if (data_len == 0)
        return NULL;
    if (size > MAX_DATA_CHUNK_SIZE)
        return NULL;

    size_t slab_class_idx = slab_allocator_find_class(alloc, size);

    if (slab_class_idx == alloc->classes_num)
        return NULL;

    return slab_class_alloc_item(&alloc->classes[slab_class_idx]);
}

size_t slab_allocator_usable_size(slab_allocator_t *alloc, size_t size)
{
    if (size == 0)
        return 0;

    if (size > MAX_DATA_CHUNK_SIZE)
        return 0;

    return alloc->classes[slab_allocator_find_class(alloc, size)].data_chunk_size;
}
