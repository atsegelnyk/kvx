#include "hashtable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "xxhash.h"

#define HASHTABLE_INIT_CAP 128
#define HASHTABLE_BUCKET_SLOTS 8

typedef enum hashtable_slot_state
{
    SLOT_EMPTY = 0,
    SLOT_BUSY = 1,
} hashtable_slot_state_t;

typedef struct hashtable_slot
{
    uint8_t state;
    uint64_t hash;
    void *value;
} hashtable_slot_t;

typedef struct hashtable_bucket
{
    size_t len;
    hashtable_slot_t slots[HASHTABLE_BUCKET_SLOTS];
    struct hashtable_bucket *next;
} hashtable_bucket_t;

void hashtable_bucket_init_chain(hashtable_bucket_t *bucket)
{
    hashtable_bucket_t *chained_bucket = calloc(1, sizeof(hashtable_bucket_t));
    if (!chained_bucket)
        abort();

    bucket->next = chained_bucket;
}

void hashtable_bucket_destroy(hashtable_bucket_t *bucket)
{
    free(bucket);
}

struct hashtable
{
    bool free_empty_chained_buckets;
    size_t len;
    size_t bucket_count;
    hashtable_bucket_t *buckets;
};

hashtable_t *hashtable_init(size_t cap, bool free_empty_chained_buckets)
{
    if (cap == 0)
        cap = HASHTABLE_INIT_CAP;

    if ((cap & (cap - 1)) != 0)
        return NULL;

    hashtable_t *table = malloc(sizeof(hashtable_t));
    if (!table)
        abort();

    hashtable_bucket_t *buckets = calloc(cap, sizeof(hashtable_bucket_t));
    if (!buckets)
        abort();

    // prefault for benchmark
    for (size_t i = 0; i < cap; i++)
    {
        buckets[i].len = 0;
    }

    table->buckets = buckets;
    table->len = 0;
    table->bucket_count = cap;
    table->free_empty_chained_buckets = free_empty_chained_buckets;

    return table;
}

void hashtable_destroy(hashtable_t *table)
{
    for (size_t i = 0; i < table->bucket_count; i++)
    {
        hashtable_bucket_t *current = table->buckets[i].next;

        while (current)
        {
            hashtable_bucket_t *next = current->next;
            free(current);
            current = next;
        }
    }

    free(table->buckets);
    free(table);
}

hashtable_slot_t *hashtable_find_empty_slot_chained(hashtable_bucket_t *bucket, hashtable_bucket_t **bucket_out)
{

    hashtable_bucket_t *current = bucket;
    hashtable_bucket_t *next = bucket->next;

    for (;;)
    {
        if (next == NULL)
        {
            hashtable_bucket_init_chain(current);
            *bucket_out = current->next;
            return &current->next->slots[0];
        }

        for (size_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
        {
            if (next->slots[i].state != SLOT_BUSY)
            {
                *bucket_out = next;
                return &next->slots[i];
            }
        }

        current = next;
        next = current->next;
    }
}

hashtable_slot_t *hashtable_find_item_slot_chained(hashtable_bucket_t *bucket, uint64_t hash, hashtable_bucket_t **bucket_out)
{

    hashtable_bucket_t *current = bucket->next;

    for (;;)
    {
        if (current == NULL)
            return NULL;

        for (size_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
        {
            if (current->slots[i].state == SLOT_BUSY &&
                current->slots[i].hash == hash)
            {
                if (bucket_out)
                    *bucket_out = current;
                return &current->slots[i];
            }
        }

        current = current->next;
    }
}

hashtable_slot_t *hashtable_find_item_slot_or_next_free_slot_chained(hashtable_bucket_t *bucket, uint64_t hash,
                                                                     hashtable_slot_t *free_slot,
                                                                     hashtable_bucket_t **bucket_out)
{
    hashtable_bucket_t *prev_bucket = bucket;
    hashtable_bucket_t *current_bucket = bucket->next;

    for (;;)
    {
        if (current_bucket == NULL)
        {
            if (free_slot)
                return free_slot;

            hashtable_bucket_init_chain(prev_bucket);

            *bucket_out = prev_bucket->next;
            return &prev_bucket->next->slots[0];
        }

        for (size_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
        {
            hashtable_slot_t *slot = &current_bucket->slots[i];

            if (slot->state == SLOT_BUSY && slot->hash == hash)
            {
                if (bucket_out)
                    *bucket_out = current_bucket;

                return slot;
            }

            if (slot->state == SLOT_EMPTY && !free_slot)
            {
                free_slot = slot;

                if (bucket_out)
                    *bucket_out = current_bucket;
            }
        }

        prev_bucket = current_bucket;
        current_bucket = current_bucket->next;
    }
}

void hashtable_free_bucket_if_chained(hashtable_t *table, hashtable_bucket_t *bucket, size_t bucket_idx)
{
    hashtable_bucket_t *parent = &table->buckets[bucket_idx];

    if (bucket == parent)
        return;

    while (parent->next)
    {
        if (parent->next == bucket)
        {
            parent->next = bucket->next;
            hashtable_bucket_destroy(bucket);
            return;
        }

        parent = parent->next;
    }
}

hashtable_slot_t *hashtable_find_empty_slot(hashtable_t *table, size_t bucket_idx, hashtable_bucket_t **bucket_out)
{
    for (size_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        if (table->buckets[bucket_idx].slots[i].state != SLOT_BUSY)
        {
            *bucket_out = &table->buckets[bucket_idx];
            return &table->buckets[bucket_idx].slots[i];
        }
    }

    return hashtable_find_empty_slot_chained(&table->buckets[bucket_idx], bucket_out);
}

hashtable_slot_t *hashtable_find_item_slot(hashtable_t *table, size_t bucket_idx, uint64_t hash, hashtable_bucket_t **bucket_out)
{
    for (size_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        if (table->buckets[bucket_idx].slots[i].state == SLOT_BUSY &&
            table->buckets[bucket_idx].slots[i].hash == hash)
        {
            if (bucket_out)
                *bucket_out = &table->buckets[bucket_idx];
            return &table->buckets[bucket_idx].slots[i];
        }
    }

    return hashtable_find_item_slot_chained(&table->buckets[bucket_idx], hash, bucket_out);
}

hashtable_slot_t *hashtable_find_item_slot_or_next_free_slot(hashtable_t *table, size_t bucket_idx, uint64_t hash, hashtable_bucket_t **bucket_out)
{

    hashtable_bucket_t *bucket = &table->buckets[bucket_idx];
    hashtable_slot_t *free_slot = NULL;

    for (size_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        hashtable_slot_t *slot = &bucket->slots[i];

        if (slot->state == SLOT_BUSY && slot->hash == hash)
        {
            if (bucket_out)
                *bucket_out = bucket;

            return slot;
        }

        if (slot->state == SLOT_EMPTY && !free_slot)
        {
            free_slot = slot;

            if (bucket_out)
                *bucket_out = bucket;
        }
    }

    return hashtable_find_item_slot_or_next_free_slot_chained(&table->buckets[bucket_idx], hash, free_slot, bucket_out);
}

bool hashtable_set(hashtable_t *table, const uint8_t *key, size_t key_len, void *value)
{
    XXH64_hash_t hash = XXH64(key, key_len, 0);
    size_t bucket_idx = (size_t)(hash & (table->bucket_count - 1));

    hashtable_bucket_t *bucket;
    hashtable_slot_t *item_slot = hashtable_find_item_slot_or_next_free_slot(table, bucket_idx, (uint64_t)hash, &bucket);

    if (item_slot->state == SLOT_EMPTY)
    {
        item_slot->state = SLOT_BUSY;
        item_slot->hash = hash;

        table->len++;
        bucket->len++;
    }

    item_slot->value = value;

    return true;
}

void *hashtable_get(hashtable_t *table, const uint8_t *key, size_t key_len)
{
    XXH64_hash_t hash = XXH64(key, key_len, 0);
    size_t bucket_idx = (size_t)(hash & (table->bucket_count - 1));

    hashtable_slot_t *item_slot = hashtable_find_item_slot(table, bucket_idx, (uint64_t)hash, NULL);
    if (!item_slot)
        return NULL;

    return item_slot->value;
}

void hashtable_delete(hashtable_t *table, const uint8_t *key, size_t key_len)
{
    XXH64_hash_t hash = XXH64(key, key_len, 0);
    size_t bucket_idx = (size_t)(hash & (table->bucket_count - 1));

    hashtable_bucket_t *bucket;
    hashtable_slot_t *item_slot = hashtable_find_item_slot(table, bucket_idx, (uint64_t)hash, &bucket);
    if (!item_slot)
        return;

    item_slot->hash = 0;
    item_slot->state = SLOT_EMPTY;
    item_slot->value = NULL;

    table->len--;
    bucket->len--;

    if (bucket->len == 0 && table->free_empty_chained_buckets)
        hashtable_free_bucket_if_chained(table, bucket, bucket_idx);
}