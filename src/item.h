#ifndef ITEM_H
#define ITEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct item_t
{
    void *slab_ptr;

    uint16_t key_len;
    uint16_t flags;

    uint32_t data_size;

    uint32_t value_len;

    uint32_t expire_at;

    uint8_t data[];
} item_t;

#define ITEM_SIZE (sizeof(item_t))

static inline uint8_t *item_key(item_t *item, size_t *len_out)
{
    *len_out = item->key_len;
    return item->data;
}

static inline uint8_t *item_value(item_t *item, size_t *len_out)
{
    *len_out = item->value_len;
    return item->data + item->key_len;
}

static inline void item_set_key(item_t *item, const uint8_t *key, size_t len)
{
    item->key_len = len;
    memcpy(item->data, key, len);
}

static inline void item_set_value(item_t *item, const uint8_t *value, size_t len)
{
    item->value_len = len;
    memcpy(item->data + item->key_len, value, len);
}

static inline bool item_key_matches(item_t *item, const uint8_t *compared_key, size_t len)
{
    if (len != item->key_len)
        return false;

    return memcmp(item->data, compared_key, len) == 0;
}

#endif // ITEM_H
