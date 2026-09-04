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

typedef struct hashtable_bucket
{
    uint8_t slot_states;
    uint64_t hashes[HASHTABLE_BUCKET_SLOTS];
    void *values[HASHTABLE_BUCKET_SLOTS];
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

uint8_t hashtable_bucket_num_free_slots(hashtable_bucket_t *bucket)
{
    return HASHTABLE_BUCKET_SLOTS - __builtin_popcount(bucket->slot_states);
}

int8_t hashtable_bucket_first_free_slot(hashtable_bucket_t *bucket)
{
    uint8_t free_bits = (uint8_t)~bucket->slot_states;
    return free_bits ? __builtin_ctz((unsigned)free_bits) : -1;
}

// slot state
void hashtable_bucket_set_slot_state(hashtable_bucket_t *bucket, int8_t slot_idx, uint8_t state)
{
    if (state)
    {
        bucket->slot_states |= (uint8_t)(1U << slot_idx);
    }
    else
    {
        bucket->slot_states &= (uint8_t)~(1U << slot_idx);
    }
}

uint8_t hashtable_bucket_get_slot_state(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return (bucket->slot_states & (uint8_t)(1U << slot_idx)) != 0;
}

// hash
void hashtable_bucket_set_slot_hash(hashtable_bucket_t *bucket, int8_t slot_idx, uint64_t hash)
{
    bucket->hashes[slot_idx] = hash;
}

uint64_t hashtable_bucket_get_slot_hash(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return bucket->hashes[slot_idx];
}

// value
void hashtable_bucket_set_slot_value(hashtable_bucket_t *bucket, int8_t slot_idx, void *value)
{
    bucket->values[slot_idx] = value;
}

void *hashtable_bucket_get_slot_value(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return bucket->values[slot_idx];
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
        volatile uint8_t *state = &buckets[i].slot_states;
        *state = 0;
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

int8_t hashtable_find_item_slot_chained(hashtable_bucket_t *bucket, uint64_t hash, hashtable_bucket_t **bucket_out)
{

    hashtable_bucket_t *current = bucket->next;

    for (;;)
    {
        if (current == NULL)
            return -1;

        for (int8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
        {
            uint8_t slot_state = hashtable_bucket_get_slot_state(current, i);
            uint64_t slot_hash = hashtable_bucket_get_slot_hash(current, i);

            if (slot_state == SLOT_BUSY && slot_hash == hash)
            {
                if (bucket_out)
                    *bucket_out = current;
                return i;
            }
        }

        current = current->next;
    }
}

int8_t hashtable_find_item_slot_or_next_free_slot_chained(hashtable_bucket_t *bucket, uint64_t hash,
                                                          int8_t free_slot,
                                                          hashtable_bucket_t **bucket_out)
{
    hashtable_bucket_t *prev_bucket = bucket;
    hashtable_bucket_t *current_bucket = bucket->next;

    for (;;)
    {
        if (current_bucket == NULL)
        {
            if (free_slot >= 0)
                return free_slot;

            hashtable_bucket_init_chain(prev_bucket);

            *bucket_out = prev_bucket->next;
            return hashtable_bucket_first_free_slot(prev_bucket->next);
        }

        for (int8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
        {
            uint8_t slot_state = hashtable_bucket_get_slot_state(current_bucket, i);
            uint64_t slot_hash = hashtable_bucket_get_slot_hash(current_bucket, i);

            if (slot_state == SLOT_BUSY && slot_hash == hash)
            {
                if (bucket_out)
                    *bucket_out = current_bucket;

                return i;
            }

            if (slot_state == SLOT_EMPTY && free_slot < 0)
            {
                free_slot = i;

                if (bucket_out)
                    *bucket_out = current_bucket;
            }
        }

        prev_bucket = current_bucket;
        current_bucket = current_bucket->next;
    }
}

int8_t hashtable_find_item_slot(hashtable_t *table, size_t bucket_idx, uint64_t hash, hashtable_bucket_t **bucket_out)
{
    for (uint8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        hashtable_bucket_t *bucket = &table->buckets[bucket_idx];

        if (hashtable_bucket_get_slot_state(bucket, i) == SLOT_BUSY &&
            hashtable_bucket_get_slot_hash(bucket, i) == hash)
        {
            if (bucket_out)
                *bucket_out = &table->buckets[bucket_idx];
            return i;
        }
    }

    return hashtable_find_item_slot_chained(&table->buckets[bucket_idx], hash, bucket_out);
}

int8_t hashtable_find_item_slot_or_next_free_slot(hashtable_t *table, size_t bucket_idx, uint64_t hash, hashtable_bucket_t **bucket_out)
{

    hashtable_bucket_t *bucket = &table->buckets[bucket_idx];
    int8_t free_slot = -1;

    for (uint8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        uint8_t slot_state = hashtable_bucket_get_slot_state(bucket, i);
        uint64_t slot_hash = hashtable_bucket_get_slot_hash(bucket, i);

        if (slot_state == SLOT_BUSY && slot_hash == hash)
        {
            if (bucket_out)
                *bucket_out = bucket;

            return i;
        }

        if (slot_state == SLOT_EMPTY && free_slot < 0)
        {
            free_slot = i;

            if (bucket_out)
                *bucket_out = bucket;
        }
    }

    return hashtable_find_item_slot_or_next_free_slot_chained(&table->buckets[bucket_idx], hash, free_slot, bucket_out);
}

bool hashtable_set(hashtable_t *table, const uint8_t *key, size_t key_len, void *value)
{

    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    size_t bucket_idx = (size_t)(hash & (table->bucket_count - 1));

    hashtable_bucket_t *bucket;
    int8_t slot_idx = hashtable_find_item_slot_or_next_free_slot(table, bucket_idx, (uint64_t)hash, &bucket);

    if (hashtable_bucket_get_slot_state(bucket, slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash(bucket, slot_idx, hash);

        table->len++;
    }

    hashtable_bucket_set_slot_value(bucket, slot_idx, value);

    return true;
}

void *hashtable_get(hashtable_t *table, const uint8_t *key, size_t key_len)
{
    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    size_t bucket_idx = (size_t)(hash & (table->bucket_count - 1));

    hashtable_bucket_t *bucket;
    int8_t slot_idx = hashtable_find_item_slot(table, bucket_idx, (uint64_t)hash, &bucket);
    if (slot_idx < 0)
        return NULL;

    return hashtable_bucket_get_slot_value(bucket, slot_idx);
}

void hashtable_delete(hashtable_t *table, const uint8_t *key, size_t key_len)
{
    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    size_t bucket_idx = (size_t)(hash & (table->bucket_count - 1));

    hashtable_bucket_t *bucket;
    int8_t slot_idx = hashtable_find_item_slot(table, bucket_idx, (uint64_t)hash, &bucket);
    if (slot_idx < 0)
        return;

    hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_EMPTY);
    hashtable_bucket_set_slot_hash(bucket, slot_idx, 0);
    hashtable_bucket_set_slot_value(bucket, slot_idx, NULL);

    table->len--;

    if (table->free_empty_chained_buckets && hashtable_bucket_num_free_slots(bucket) == HASHTABLE_BUCKET_SLOTS)
        hashtable_free_bucket_if_chained(table, bucket, bucket_idx);
}
