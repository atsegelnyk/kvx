#include "hashtable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "item.h"
#include "pages.h"
#include "xxhash.h"

#define HASHTABLE_INIT_CAP 128
#define HASHTABLE_BUCKET_SLOTS 8
#define HASHTABLE_SCALE_AT_LOAD_FACTOR (6.0 / 8.0)

#define TAG_LSBS 0x0101010101010101ULL   // bit 0 of every byte
#define TAG_MSBS 0x8080808080808080ULL   // bit 7 of every byte
#define TAG_GATHER 0x0002040810204081ULL // moves bit 8i+7 -> bit 56+i

#define SHARD_BITS 8

typedef enum hashtable_slot_state
{
    SLOT_EMPTY = 0,
    SLOT_BUSY = 1,
} hashtable_slot_state_t;

typedef struct hashtable_bucket
{
    uint8_t slot_states;
    uint64_t hash_tags;
    item_t *items[HASHTABLE_BUCKET_SLOTS];
    struct hashtable_bucket *next;
} hashtable_bucket_t;

static void hashtable_bucket_init_chain(hashtable_bucket_t *bucket)
{
    hashtable_bucket_t *chained_bucket = calloc(1, sizeof(hashtable_bucket_t));
    if (!chained_bucket)
        abort();

    bucket->next = chained_bucket;
}

static inline uint8_t hashtable_bucket_match_tag(const hashtable_bucket_t *bucket, uint8_t tag)
{
    uint64_t x = bucket->hash_tags ^ (TAG_LSBS * (uint64_t)tag);

    uint64_t zero = (x - TAG_LSBS) & ~x & TAG_MSBS; // bit 8i+7 set iff byte i == tag

    uint8_t cand = (uint8_t)((zero * TAG_GATHER) >> 56);

    return cand & bucket->slot_states;
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

static inline int8_t hashtable_bucket_first_free_slot(hashtable_bucket_t *bucket)
{
    uint8_t free_bits = (uint8_t)~bucket->slot_states;
    return free_bits ? __builtin_ctz((unsigned)free_bits) : -1;
}

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

// hash tag
static inline void hashtable_bucket_set_slot_hash_tag(hashtable_bucket_t *bucket, int8_t slot, uint8_t tag)
{
    const uint64_t shift = (uint64_t)slot * 8;
    const uint64_t mask = 0xffULL << shift;

    bucket->hash_tags =
        (bucket->hash_tags & ~mask) |
        ((uint64_t)tag << shift);
}

// value
static inline void hashtable_bucket_set_slot_item(hashtable_bucket_t *bucket, int8_t slot_idx, item_t *item)
{
    bucket->items[slot_idx] = item;
}

static inline item_t *hashtable_bucket_get_slot_item(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return bucket->items[slot_idx];
}

static inline item_t **hashtable_bucket_get_slot_item_ptr(hashtable_bucket_t *bucket, int8_t slot_idx)
{
    return &bucket->items[slot_idx];
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
    hashtable_bucket_t *buckets = pages_map(cap * sizeof(*buckets), &bytes);
    if (!buckets)
    {
        free(table);
        return NULL;
    }

    pages_populate(buckets, 0, bytes, bytes);

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

    pages_unmap(buckets, bytes);
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
static int8_t hashtable_find_item_slot_in_bucket_chained(hashtable_bucket_t *bucket, uint8_t tag, const uint8_t *key, size_t key_len, hashtable_bucket_t **bucket_out)
{

    hashtable_bucket_t *current = bucket->next;

    for (;;)
    {
        if (current == NULL)
            return -1;

        uint8_t candidates = hashtable_bucket_match_tag(current, tag);
        while (candidates)
        {
            int8_t slot_idx = __builtin_ctz(candidates);
            candidates &= candidates - 1;

            item_t *item = current->items[slot_idx];

            if (item_key_matches(item, key, key_len))
            {
                *bucket_out = current;
                return slot_idx;
            }
        }

        current = current->next;
    }
}

static int8_t hashtable_find_item_slot_in_bucket(hashtable_bucket_t *bucket, uint8_t tag, const uint8_t *key, size_t key_len, hashtable_bucket_t **bucket_out)
{
    uint8_t candidates = hashtable_bucket_match_tag(bucket, tag);
    while (candidates)
    {
        int8_t slot = __builtin_ctz(candidates);
        candidates &= candidates - 1;

        item_t *item = bucket->items[slot];

        if (item_key_matches(item, key, key_len))
        {
            *bucket_out = bucket;
            return slot;
        }
    }

    return hashtable_find_item_slot_in_bucket_chained(bucket, tag, key, key_len, bucket_out);
}

// find item slot or next free slot
static int8_t hashtable_find_item_slot_or_next_free_slot_chained(hashtable_bucket_t *bucket, uint8_t tag, const uint8_t *key, size_t key_len,
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

            // first slot in a new chained bucket
            return 0;
        }

        uint8_t candidates = hashtable_bucket_match_tag(current_bucket, tag);
        while (candidates)
        {
            int8_t slot = __builtin_ctz(candidates);
            candidates &= candidates - 1;

            item_t *item = current_bucket->items[slot];

            if (item_key_matches(item, key, key_len))
            {
                *bucket_out = current_bucket;
                return slot;
            }
        }

        if (free_slot < 0)
        {
            free_slot = hashtable_bucket_first_free_slot(current_bucket);
            if (free_slot >= 0)
                *bucket_out = current_bucket;
        }

        prev_bucket = current_bucket;
        current_bucket = current_bucket->next;
    }
}

static int8_t hashtable_find_item_slot_or_next_free_slot_in_bucket(hashtable_bucket_t *bucket, uint8_t tag, const uint8_t *key, size_t key_len, hashtable_bucket_t **bucket_out)
{
    uint8_t candidates = hashtable_bucket_match_tag(bucket, tag);
    while (candidates)
    {
        int8_t slot = __builtin_ctz(candidates);
        candidates &= candidates - 1;

        item_t *item = bucket->items[slot];

        if (item_key_matches(item, key, key_len))
        {
            *bucket_out = bucket;
            return slot;
        }
    }

    int8_t free_slot = hashtable_bucket_first_free_slot(bucket);
    if (free_slot >= 0)
        *bucket_out = bucket;

    return hashtable_find_item_slot_or_next_free_slot_chained(bucket, tag, key, key_len, free_slot, bucket_out);
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
    return ((hash >> SHARD_BITS) & (table->old_bucket_count - 1)) < table->migration_pos;
}

static void hashtable_check_init_migration(hashtable_t *table)
{
    if (table->old_size < table->scale_at_size || table->new_buckets)
    {
        return;
    }

    size_t new_cap = table->old_bucket_count * 2;
    size_t bytes;
    hashtable_bucket_t *new_buckets = pages_map(new_cap * sizeof(*new_buckets), &bytes);
    if (!new_buckets)
        return;

    pages_populate(new_buckets, 0, bytes, bytes);

    table->new_buckets = new_buckets;
    table->new_bucket_count = new_cap;
    table->new_bytes = bytes;
}

static bool hashtable_finish_migration(hashtable_t *table)
{

    pages_unmap(table->old_buckets, table->old_bytes);

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

static void hashtable_migrate_slot(hashtable_t *table, int8_t old_slot_idx, hashtable_bucket_t *old_bucket, item_t *item)
{
    hashtable_bucket_set_slot_state(old_bucket, old_slot_idx, SLOT_EMPTY);
    table->old_size--;

    // rehash
    size_t key_len;
    uint8_t *key = item_key(item, &key_len);
    XXH64_hash_t hash = XXH3_64bits(key, key_len);
    uint8_t tag = (uint8_t)(hash >> 56);

    hashtable_bucket_t *new_bucket;
    size_t new_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->new_bucket_count - 1));
    int8_t new_slot_idx = hashtable_find_item_slot_or_next_free_slot_in_bucket(&table->new_buckets[new_bucket_idx], tag, key, key_len, &new_bucket);

    if (hashtable_bucket_get_slot_state(new_bucket, new_slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(new_bucket, new_slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash_tag(new_bucket, new_slot_idx, tag);
        hashtable_bucket_set_slot_item(new_bucket, new_slot_idx, item);
        table->new_size++;
    }
}

static void hashtable_migrate_slots(hashtable_t *table, hashtable_bucket_t *bucket)
{

    for (uint8_t i = 0; i < HASHTABLE_BUCKET_SLOTS; i++)
    {
        if (hashtable_bucket_get_slot_state(bucket, i) != SLOT_BUSY)
            continue;

        hashtable_migrate_slot(table, i, bucket, hashtable_bucket_get_slot_item(bucket, i));
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

static item_t **hashtable_set_old(hashtable_t *table, uint64_t hash, uint8_t tag, const uint8_t *key, size_t key_len)
{
    hashtable_bucket_t *bucket;
    size_t old_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->old_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_or_next_free_slot_in_bucket(&table->old_buckets[old_bucket_idx], tag, key, key_len, &bucket);

    item_t **item_slot_ptr = hashtable_bucket_get_slot_item_ptr(bucket, slot_idx);

    if (hashtable_bucket_get_slot_state(bucket, slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash_tag(bucket, slot_idx, tag);

        table->old_size++;
    }

    return item_slot_ptr;
    // hashtable_bucket_set_slot_item(bucket, slot_idx, item);
}

static item_t **hashtable_set_new(hashtable_t *table, uint64_t hash, uint8_t tag, const uint8_t *key, size_t key_len)
{
    hashtable_bucket_t *bucket;
    size_t new_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->new_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_or_next_free_slot_in_bucket(&table->new_buckets[new_bucket_idx], tag, key, key_len, &bucket);

    item_t **item_slot_ptr = hashtable_bucket_get_slot_item_ptr(bucket, slot_idx);

    if (hashtable_bucket_get_slot_state(bucket, slot_idx) == SLOT_EMPTY)
    {
        hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_BUSY);
        hashtable_bucket_set_slot_hash_tag(bucket, slot_idx, tag);

        table->new_size++;
    }

    return item_slot_ptr;
    // hashtable_bucket_set_slot_item(bucket, slot_idx, item);
}

item_t **hashtable_set(hashtable_t *table, uint64_t hash, const uint8_t *key, size_t key_len)
{
    hashtable_maintenance(table);

    uint8_t tag = (uint8_t)(hash >> 56);

    if (hashtable_item_in_new_bucket(table, hash))
    {
        return hashtable_set_new(table, hash, tag, key, key_len);
    }

    hashtable_check_init_migration(table);

    return hashtable_set_old(table, hash, tag, key, key_len);
}

static item_t *hashtable_get_old(hashtable_t *table, uint64_t hash, uint8_t tag, const uint8_t *key, size_t key_len)
{
    hashtable_bucket_t *bucket;
    size_t old_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->old_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->old_buckets[old_bucket_idx], tag, key, key_len, &bucket);
    if (slot_idx < 0)
        return NULL;

    return hashtable_bucket_get_slot_item(bucket, slot_idx);
}

static item_t *hashtable_get_new(hashtable_t *table, uint64_t hash, uint8_t tag, const uint8_t *key, size_t key_len)
{
    hashtable_bucket_t *bucket;
    size_t new_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->new_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->new_buckets[new_bucket_idx], tag, key, key_len, &bucket);
    if (slot_idx < 0)
        return NULL;

    return hashtable_bucket_get_slot_item(bucket, slot_idx);
}

item_t *hashtable_get(hashtable_t *table, uint64_t hash, const uint8_t *key, size_t key_len)
{
    uint8_t tag = (uint8_t)(hash >> 56);

    if (hashtable_item_in_new_bucket(table, hash))
    {
        return hashtable_get_new(table, hash, tag, key, key_len);
    }

    return hashtable_get_old(table, hash, tag, key, key_len);
}

static item_t *hashtable_delete_old(hashtable_t *table, uint64_t hash, uint8_t tag, const uint8_t *key, size_t key_len)
{
    hashtable_bucket_t *bucket;
    size_t old_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->old_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->old_buckets[old_bucket_idx], tag, key, key_len, &bucket);
    if (slot_idx < 0)
        return NULL;

    item_t *item = hashtable_bucket_get_slot_item(bucket, slot_idx);

    hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_EMPTY);
    hashtable_bucket_set_slot_hash_tag(bucket, slot_idx, 0);
    hashtable_bucket_set_slot_item(bucket, slot_idx, NULL);
    table->old_size--;

    return item;
}

static item_t *hashtable_delete_new(hashtable_t *table, uint64_t hash, uint8_t tag, const uint8_t *key, size_t key_len)
{
    hashtable_bucket_t *bucket;
    size_t new_bucket_idx = (size_t)((hash >> SHARD_BITS) & (table->new_bucket_count - 1));
    int8_t slot_idx = hashtable_find_item_slot_in_bucket(&table->new_buckets[new_bucket_idx], tag, key, key_len, &bucket);
    if (slot_idx < 0)
        return NULL;

    item_t *item = hashtable_bucket_get_slot_item(bucket, slot_idx);

    hashtable_bucket_set_slot_state(bucket, slot_idx, SLOT_EMPTY);
    hashtable_bucket_set_slot_hash_tag(bucket, slot_idx, 0);
    hashtable_bucket_set_slot_item(bucket, slot_idx, NULL);
    table->new_size--;

    return item;
}

item_t *hashtable_delete(hashtable_t *table, uint64_t hash, const uint8_t *key, size_t key_len)
{
    hashtable_maintenance(table);

    uint8_t tag = (uint8_t)(hash >> 56);

    if (hashtable_item_in_new_bucket(table, hash))
    {
        return hashtable_delete_new(table, hash, tag, key, key_len);
    }

    return hashtable_delete_old(table, hash, tag, key, key_len);
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
