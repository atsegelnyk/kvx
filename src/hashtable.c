#include "hashtable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "hashtable_mem.h"
#include "xxhash.h"

#define HASHTABLE_INIT_CAP 128
#define HASHTABLE_BUCKET_SLOTS 8
#define HASHTABLE_SCALE_AT_LOAD_FACTOR 6 / 8

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

static void hashtable_bucket_init_chain(hashtable_bucket_t *bucket)
{
    hashtable_bucket_t *chained_bucket = calloc(1, sizeof(hashtable_bucket_t));
    if (!chained_bucket)
        abort();

    bucket->next = chained_bucket;
}

// static inline uint8_t hashtable_bucket_num_free_slots(hashtable_bucket_t *bucket)
// {
//     return HASHTABLE_BUCKET_SLOTS - __builtin_popcount(bucket->slot_states);
// }

// static inline bool hashtable_bucket_has_free_slots(hashtable_bucket_t *bucket)
// {
//     return hashtable_bucket_num_free_slots(bucket) > 0;
// }

// static inline bool hashtable_bucket_empty(hashtable_bucket_t *bucket)
// {
//     return hashtable_bucket_num_free_slots(bucket) == HASHTABLE_BUCKET_SLOTS;
// }

// static inline int8_t hashtable_bucket_first_free_slot(hashtable_bucket_t *bucket)
// {
//     uint8_t free_bits = (uint8_t)~bucket->slot_states;
//     return free_bits ? __builtin_ctz((unsigned)free_bits) : -1;
// }

// slot state
static inline void hashtable_bucket_set_slot_state(hashtable_bucket_t *bucket, int8_t slot_idx, uint8_t state)
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

static inline uint8_t hashtable_bucket_get_slot_state(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return (bucket->slot_states & (uint8_t)(1U << slot_idx)) != 0;
}

// hash
static inline void hashtable_bucket_set_slot_hash(hashtable_bucket_t *bucket, int8_t slot_idx, uint64_t hash)
{
    bucket->hashes[slot_idx] = hash;
}

static inline uint64_t hashtable_bucket_get_slot_hash(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return bucket->hashes[slot_idx];
}

// value
static inline void hashtable_bucket_set_slot_value(hashtable_bucket_t *bucket, int8_t slot_idx, void *value)
{
    bucket->values[slot_idx] = value;
}

static inline void *hashtable_bucket_get_slot_value(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return bucket->values[slot_idx];
}

struct hashtable
{
    size_t scale_at_size;

    size_t migration_pos;

    size_t old_size;
    size_t old_bytes;
    size_t old_bucket_count;
    hashtable_bucket_t *old_buckets;

    size_t new_size;
    size_t new_bytes;
    size_t new_bucket_count;
    hashtable_bucket_t *new_buckets;
};

hashtable_t *hashtable_init(size_t cap)
{
    if (cap == 0)
        cap = HASHTABLE_INIT_CAP;

    if ((cap & (cap - 1)) != 0)
        return NULL;

    hashtable_t *table = malloc(sizeof(hashtable_t));
    if (!table)
        abort();

    size_t bytes;
    hashtable_bucket_t *buckets = ht_alloc_pages(cap * sizeof(*buckets), &bytes);
    if (!buckets)
    {
        free(table);
        return NULL;
    }

    ht_populate(buckets, 0, bytes, bytes);

    table->scale_at_size = (cap * HASHTABLE_BUCKET_SLOTS) * HASHTABLE_SCALE_AT_LOAD_FACTOR;
    table->migration_pos = 0;

    table->old_size = 0;
    table->old_bytes = bytes;
    table->old_buckets = buckets;
    table->old_bucket_count = cap;

    table->new_size = 0;
    table->new_buckets = NULL;
    table->new_bucket_count = 0;

    return table;
}

static void hashtable_destroy_buckets(hashtable_bucket_t *buckets, size_t count, size_t bytes)
{
    for (size_t i = 0; i < count; i++)
    {
        hashtable_bucket_t *current = buckets[i].next;

        while (current)
        {
            hashtable_bucket_t *next = current->next;
            free(current);
            current = next;
        }
    }

    ht_free_pages(buckets, bytes);
}

void hashtable_destroy(hashtable_t *table)
{
    if (table->new_buckets)
    {
        hashtable_destroy_buckets(table->new_buckets, table->new_bucket_count, table->new_bytes);
    }

    hashtable_destroy_buckets(table->old_buckets, table->old_bucket_count, table->old_bytes);
    free(table);
}

// find item slot
static int8_t hashtable_find_item_slot_in_bucket_chained(hashtable_bucket_t *bucket, uint64_t hash, hashtable_bucket_t **bucket_out)
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
                *bucket_out = current;
                return i;
            }
        }

        current = current->next;
    }
}

static int8_t hashtable_find_item_slot_in_bucket(hashtable_bucket_t *bucket, uint64_t hash, hashtable_bucket_t **bucket_out)
{
    for (uint8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        if (hashtable_bucket_get_slot_state(bucket, i) == SLOT_BUSY &&
            hashtable_bucket_get_slot_hash(bucket, i) == hash)
        {
            *bucket_out = bucket;
            return i;
        }
    }

    return hashtable_find_item_slot_in_bucket_chained(bucket, hash, bucket_out);
}

// find item slot or next free slot
static int8_t hashtable_find_item_slot_or_next_free_slot_chained(hashtable_bucket_t *bucket, uint64_t hash,
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
            return 0;
            // return hashtable_bucket_first_free_slot(prev_bucket->next);
        }

        for (int8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
        {
            uint8_t slot_state = hashtable_bucket_get_slot_state(current_bucket, i);
            uint64_t slot_hash = hashtable_bucket_get_slot_hash(current_bucket, i);

            if (slot_state == SLOT_BUSY && slot_hash == hash)
            {
                *bucket_out = current_bucket;
                return i;
            }

            if (slot_state == SLOT_EMPTY && free_slot < 0)
            {
                free_slot = i;
                *bucket_out = current_bucket;
            }
        }

        prev_bucket = current_bucket;
        current_bucket = current_bucket->next;
    }
}

static int8_t hashtable_find_item_slot_or_next_free_slot_in_bucket(hashtable_bucket_t *bucket, uint64_t hash, hashtable_bucket_t **bucket_out)
{
    int8_t free_slot = -1;

    for (uint8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        uint8_t slot_state = hashtable_bucket_get_slot_state(bucket, i);
        uint64_t slot_hash = hashtable_bucket_get_slot_hash(bucket, i);

        if (slot_state == SLOT_BUSY && slot_hash == hash)
        {
            *bucket_out = bucket;
            return i;
        }

        if (slot_state == SLOT_EMPTY && free_slot < 0)
        {
            free_slot = i;
            *bucket_out = bucket;
        }
    }

    return hashtable_find_item_slot_or_next_free_slot_chained(bucket, hash, free_slot, bucket_out);
}

// table migration
static inline bool hashtable_migration_running(hashtable_t *table)
{
    return table->new_buckets != 0;
}

static inline bool hashtable_item_in_new_bucket(hashtable_t *table, uint64_t hash)
{
    if (!table->new_buckets)
        return false;
    return (hash & (table->old_bucket_count - 1)) < table->migration_pos;
}

static void hashtable_check_init_migration(hashtable_t *table)
{
    if (table->old_size < table->scale_at_size || table->new_buckets)
    {
        return;
    }

    size_t new_cap = table->old_bucket_count * 2;
    size_t bytes;
    hashtable_bucket_t *new_buckets = ht_alloc_pages(new_cap * sizeof(*new_buckets), &bytes);
    if (!new_buckets)
        return;

    table->new_buckets = new_buckets;
    table->new_bucket_count = new_cap;
    table->new_bytes = bytes;
}

static bool hashtable_finish_migration(hashtable_t *table)
{

    ht_free_pages(table->old_buckets, table->old_bytes);

    table->migration_pos = 0;
    table->scale_at_size = (table->new_bucket_count * HASHTABLE_BUCKET_SLOTS) * HASHTABLE_SCALE_AT_LOAD_FACTOR;

    table->old_buckets = table->new_buckets;
    table->old_bucket_count = table->new_bucket_count;
    table->old_size = table->new_size;

    table->new_buckets = NULL;
    table->new_bucket_count = 0;
    table->new_size = 0;

    return true;
}

static void hashtable_migrate_slot(hashtable_t *table, int8_t old_slot_idx, hashtable_bucket_t *old_bucket, uint64_t hash, void *value)
{
    hashtable_bucket_set_slot_state(old_bucket, old_slot_idx, SLOT_EMPTY);
    table->old_size--;

    hashtable_bucket_t *new_bucket;
    size_t new_bucket_idx = (size_t)(hash & (table->new_bucket_count - 1));
    int8_t new_slot_idx = hashtable_find_item_slot_or_next_free_slot_in_bucket(&table->new_buckets[new_bucket_idx], hash, &new_bucket);

    if (hashtable_bucket_get_slot_state(new_bucket, new_slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(new_bucket, new_slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash(new_bucket, new_slot_idx, hash);
        hashtable_bucket_set_slot_value(new_bucket, new_slot_idx, value);
        table->new_size++;
    }
}

static void hashtable_migrate_slots(hashtable_t *table, hashtable_bucket_t *bucket)
{

    for (uint8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        if (hashtable_bucket_get_slot_state(bucket, i) != SLOT_BUSY)
            continue;

        hashtable_migrate_slot(table, i, bucket,
                               hashtable_bucket_get_slot_hash(bucket, i),
                               hashtable_bucket_get_slot_value(bucket, i));
    }
}

static void hashtable_migrate_bucket(hashtable_t *table, hashtable_bucket_t *bucket)
{
    hashtable_migrate_slots(table, bucket);

    hashtable_bucket_t *current = bucket->next;
    bucket->next = NULL;

    while (current)
    {
        hashtable_bucket_t *next = current->next;

        hashtable_migrate_slots(table, current);
        free(current);
        current = next;
    }
}

static void hashtable_process_migration_step(hashtable_t *table)
{
    hashtable_migrate_bucket(table, &table->old_buckets[table->migration_pos++]);

    if (table->migration_pos == table->old_bucket_count)
        hashtable_finish_migration(table);
}

static inline void hashtable_maintenance(hashtable_t *table)
{
    if (hashtable_migration_running(table))
        hashtable_process_migration_step(table);
}

// public api

static void hashtable_set_old(hashtable_t *table, uint64_t hash, void *value)
{
    hashtable_bucket_t *bucket;
    size_t old_bucket_idx = (size_t)(hash & (table->old_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_or_next_free_slot_in_bucket(&table->old_buckets[old_bucket_idx], hash, &bucket);

    if (hashtable_bucket_get_slot_state(bucket, slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash(bucket, slot_idx, hash);

        table->old_size++;
    }

    hashtable_bucket_set_slot_value(bucket, slot_idx, value);
}

static void hashtable_set_new(hashtable_t *table, uint64_t hash, void *value)
{
    hashtable_bucket_t *bucket;
    size_t new_bucket_idx = (size_t)(hash & (table->new_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_or_next_free_slot_in_bucket(&table->new_buckets[new_bucket_idx], hash, &bucket);

    if (hashtable_bucket_get_slot_state(bucket, slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash(bucket, slot_idx, hash);

        table->new_size++;
    }

    hashtable_bucket_set_slot_value(bucket, slot_idx, value);
}

void hashtable_set(hashtable_t *table, const uint8_t *key, size_t key_len, void *value)
{
    hashtable_maintenance(table);

    XXH64_hash_t hash = XXH3_64bits(key, key_len);

    if (hashtable_item_in_new_bucket(table, hash))
    {
        hashtable_set_new(table, hash, value);
        return;
    }

    hashtable_set_old(table, hash, value);

    hashtable_check_init_migration(table);
}

static void *hashtable_get_old(hashtable_t *table, uint64_t hash)
{
    hashtable_bucket_t *bucket;
    size_t old_bucket_idx = (size_t)(hash & (table->old_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->old_buckets[old_bucket_idx], hash, &bucket);
    if (slot_idx < 0)
        return NULL;

    return hashtable_bucket_get_slot_value(bucket, slot_idx);
}

static void *hashtable_get_new(hashtable_t *table, uint64_t hash)
{
    hashtable_bucket_t *bucket;
    size_t new_bucket_idx = (size_t)(hash & (table->new_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->new_buckets[new_bucket_idx], hash, &bucket);
    if (slot_idx < 0)
        return NULL;

    return hashtable_bucket_get_slot_value(bucket, slot_idx);
}

void *hashtable_get(hashtable_t *table, const uint8_t *key, size_t key_len)
{
    // hashtable_maintenance(table);

    XXH64_hash_t hash = XXH3_64bits(key, key_len);

    if (hashtable_item_in_new_bucket(table, hash))
    {
        return hashtable_get_new(table, hash);
    }

    return hashtable_get_old(table, hash);
}

static void hashtable_delete_old(hashtable_t *table, uint64_t hash)
{
    hashtable_bucket_t *bucket;
    size_t old_bucket_idx = (size_t)(hash & (table->old_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->old_buckets[old_bucket_idx], hash, &bucket);
    if (slot_idx < 0)
        return;

    hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_EMPTY);
    hashtable_bucket_set_slot_hash(bucket, slot_idx, 0);
    hashtable_bucket_set_slot_value(bucket, slot_idx, NULL);
    table->old_size--;
}

static void hashtable_delete_new(hashtable_t *table, uint64_t hash)
{
    hashtable_bucket_t *bucket;
    size_t new_bucket_idx = (size_t)(hash & (table->new_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->new_buckets[new_bucket_idx], hash, &bucket);
    if (slot_idx < 0)
        return;

    hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_EMPTY);
    hashtable_bucket_set_slot_hash(bucket, slot_idx, 0);
    hashtable_bucket_set_slot_value(bucket, slot_idx, NULL);
    table->new_size--;
}

void hashtable_delete(hashtable_t *table, const uint8_t *key, size_t key_len)
{
    hashtable_maintenance(table);

    XXH64_hash_t hash = XXH3_64bits(key, key_len);

    if (hashtable_item_in_new_bucket(table, hash))
    {
        hashtable_delete_new(table, hash);
        return;
    }

    hashtable_delete_old(table, hash);
}

size_t hashtable_len(const hashtable_t *table)
{
    return table->old_size + table->new_size;
}

size_t hashtable_bucket_count(const hashtable_t *table)
{
    return table->new_buckets ? table->new_bucket_count
                              : table->old_bucket_count;
}

int hashtable_migrating(const hashtable_t *table)
{
    return table->new_buckets != NULL;
}