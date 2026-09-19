#include "kv.h"

#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"
#include "item.h"
#include "slab_alloc.h"
#include "xxhash.h"

#define KV_SHARD_TABLE_INITIAL_SIZE 128

#define KV_SHARD_NUM 256
#define KV_SHARD_MASK 255

typedef struct kv_shard_t
{
    _Alignas(64) pthread_mutex_t mu;

    hashtable_t *table;
    slab_allocator_t *allocator;
} kv_shard_t;

static void kv_shard_init(kv_shard_t *shard)
{
    shard->allocator = slab_allocator_init();
    shard->table = hashtable_init(KV_SHARD_TABLE_INITIAL_SIZE);
    pthread_mutex_init(&shard->mu, NULL);
}

static void kv_shard_destroy(kv_shard_t *shard)
{
    slab_allocator_destroy(shard->allocator);
    hashtable_destroy(shard->table);
    pthread_mutex_destroy(&shard->mu);
}

static kv_error_t kv_shard_get(kv_shard_t *shard,
                               uint64_t hash,
                               const uint8_t *key,
                               size_t key_len,
                               uint8_t *dst,
                               size_t dst_len,
                               size_t *value_len_out,
                               uint16_t *flags_out,
                               uint32_t *exp_out)
{
    pthread_mutex_lock(&shard->mu);

    item_t *item = hashtable_get(shard->table, hash, key, key_len);
    if (!item)
    {
        pthread_mutex_unlock(&shard->mu);
        return KV_ERR_NOT_FOUND;
    }

    size_t value_len;
    uint8_t *value = item_value(item, &value_len);

    *value_len_out = value_len;
    if (item->value_len > dst_len)
    {
        pthread_mutex_unlock(&shard->mu);
        return KV_ERR_VALUE_LEN_EXCEEDS_DST_BUFFER_SIZE;
    }

    *flags_out = item->flags;
    *exp_out = item->expire_at;
    memcpy(dst, value, value_len);

    pthread_mutex_unlock(&shard->mu);

    return KV_OK;
}

static kv_error_t kv_shard_set(kv_shard_t *shard,
                               uint64_t hash,
                               const uint8_t *key,
                               size_t key_len,
                               const uint8_t *value,
                               size_t value_len,
                               uint16_t flags,
                               uint32_t exp)
{
    pthread_mutex_lock(&shard->mu);

    size_t data_len = key_len + value_len;

    item_t **item_slot_ptr = hashtable_set(shard->table, hash, key, key_len);
    item_t *item = *item_slot_ptr;

    if (!item)
    {
        item = slab_allocator_alloc_item(shard->allocator, data_len);
        if (!item)
        {
            hashtable_delete(shard->table, hash, key, key_len);
            pthread_mutex_unlock(&shard->mu);
            return KV_ERR_NO_MEM;
        }

        *item_slot_ptr = item;
    }

    if (item->data_size < data_len)
    {
        item_t *fresh = slab_allocator_alloc_item(shard->allocator, data_len);
        if (!fresh)
        {
            pthread_mutex_unlock(&shard->mu);
            return KV_ERR_NO_MEM;
        }

        slab_allocator_free_item(item);
        item = fresh;
        *item_slot_ptr = item;
    }

    item->expire_at = exp;
    item->flags = flags;

    item_set_key(item, key, key_len);
    item_set_value(item, value, value_len);

    pthread_mutex_unlock(&shard->mu);

    return KV_OK;
}

static kv_error_t kv_shard_delete(kv_shard_t *shard,
                                  uint64_t hash,
                                  const uint8_t *key,
                                  size_t key_len)
{
    pthread_mutex_lock(&shard->mu);

    item_t *item = hashtable_delete(shard->table, hash, key, key_len);
    if (!item)
    {
        pthread_mutex_unlock(&shard->mu);
        return KV_ERR_NOT_FOUND;
    }

    slab_allocator_free_item(item);
    pthread_mutex_unlock(&shard->mu);

    return KV_OK;
}

struct kv_t
{
    kv_shard_t shards[256];
};

kv_t *kv_init(void)
{
    kv_t *kv = aligned_alloc(alignof(kv_t), sizeof(kv_t));
    if (!kv)
        abort();

    for (size_t i = 0; i < 256; i++)
    {
        kv_shard_init(&kv->shards[i]);
    }

    return kv;
}

void kv_destroy(kv_t *kv)
{
    for (size_t i = 0; i < 256; i++)
    {
        kv_shard_destroy(&kv->shards[i]);
    }

    free(kv);
}

kv_error_t kv_get(kv_t *kv,
                  const uint8_t *key,
                  size_t key_len,
                  uint8_t *dst,
                  size_t dst_len,
                  size_t *value_len_out,
                  uint16_t *flags_out,
                  uint32_t *exp_out)
{
    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    uint8_t shard_idx = (uint8_t)(hash & KV_SHARD_MASK);

    return kv_shard_get(&kv->shards[shard_idx], hash, key, key_len, dst, dst_len, value_len_out, flags_out, exp_out);
}

kv_error_t kv_set(kv_t *kv,
                  const uint8_t *key,
                  size_t key_len,
                  const uint8_t *value,
                  size_t value_len,
                  uint16_t flags,
                  uint32_t exp)
{
    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    uint8_t shard_idx = (uint8_t)(hash & KV_SHARD_MASK);

    return kv_shard_set(&kv->shards[shard_idx], hash, key, key_len, value, value_len, flags, exp);
}

kv_error_t kv_delete(kv_t *kv,
                     const uint8_t *key,
                     size_t key_len)
{
    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    uint8_t shard_idx = (uint8_t)(hash & KV_SHARD_MASK);

    return kv_shard_delete(&kv->shards[shard_idx], hash, key, key_len);
}
