#define _GNU_SOURCE

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"
#include "item.h"
#include "xxhash.h"

/* ------------------------------------------------------------ framework */

static int g_failures;
static int g_checks;
static const char *g_current_test;

#define CHECK(cond)                                             \
    do                                                          \
    {                                                           \
        g_checks++;                                             \
        if (!(cond))                                            \
        {                                                       \
            g_failures++;                                       \
            fprintf(stderr, "  FAIL %s:%d in %s: %s\n",         \
                    __FILE__, __LINE__, g_current_test, #cond); \
        }                                                       \
    } while (0)

#define CHECK_EQ_SIZE(actual, expected)                                      \
    do                                                                       \
    {                                                                        \
        g_checks++;                                                          \
        size_t a_ = (actual), e_ = (expected);                               \
        if (a_ != e_)                                                        \
        {                                                                    \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d in %s: %s == %zu, expected %zu\n", \
                    __FILE__, __LINE__, g_current_test, #actual, a_, e_);    \
        }                                                                    \
    } while (0)

static void pool_reset(void);

#define RUN(fn)                                         \
    do                                                  \
    {                                                   \
        g_current_test = #fn;                           \
        int before = g_failures;                        \
        fn();                                           \
        pool_reset();                                   \
        printf("%-42s %s\n", #fn,                       \
               g_failures == before ? "ok" : "FAILED"); \
    } while (0)

/* ----------------------------------------------------------------- util */

static inline uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Deterministic key for index i. Distinct lengths exercise the hash on
   short, word-aligned and long inputs within one run. */
typedef struct
{
    uint8_t bytes[64];
    size_t len;
} tkey_t;

static tkey_t make_key(size_t i)
{
    tkey_t k;
    uint64_t state = 0xC0FFEE00 + i;

    k.len = 8 + (i % 40);
    for (size_t off = 0; off < k.len; off += 8)
    {
        uint64_t r = splitmix64(&state);
        size_t chunk = (k.len - off < 8) ? k.len - off : 8;
        memcpy(k.bytes + off, &r, chunk);
    }
    return k;
}

/* ------------------------------------------------------------ item pool

   item_t has a flexible array member and no constructor, so the test owns
   item storage. Every item carries one uint64_t payload, self-identifying so
   a lookup returning the wrong entry is caught rather than just a wrong count.

   Ownership: the table stores these pointers and must not free them.
   pool_reset() runs after each test; if hashtable_destroy also frees items,
   that shows up immediately as a double free under ASan. */

typedef struct item_pool
{
    item_t **items;
    size_t count;
    size_t cap;
} item_pool_t;

static item_pool_t g_pool;

/* Returned by fetch helpers when the lookup missed. Distinct from every
   value the tests store. */
#define NO_ITEM SIZE_MAX

static item_t *make_item(const uint8_t *key, size_t key_len, uint64_t value)
{
    item_t *item = malloc(sizeof(item_t) + key_len + sizeof value);

    if (!item)
    {
        fprintf(stderr, "out of memory allocating item\n");
        exit(EXIT_FAILURE);
    }

    item->key_len = (uint16_t)key_len;
    item->flags = 0;
    item->value_len = (uint32_t)sizeof value;
    item->expire_at = 0;

    if (key_len)
        item_set_key(item, key, key_len);
    item_set_value(item, (const uint8_t *)&value, sizeof value);

    if (g_pool.count == g_pool.cap)
    {
        size_t cap = g_pool.cap ? g_pool.cap * 2 : 1024;
        item_t **grown = realloc(g_pool.items, cap * sizeof(item_t *));

        if (!grown)
        {
            fprintf(stderr, "out of memory growing item pool\n");
            exit(EXIT_FAILURE);
        }

        g_pool.items = grown;
        g_pool.cap = cap;
    }

    g_pool.items[g_pool.count++] = item;
    return item;
}

static void pool_reset(void)
{
    for (size_t i = 0; i < g_pool.count; i++)
        free(g_pool.items[i]);

    free(g_pool.items);
    g_pool.items = NULL;
    g_pool.count = 0;
    g_pool.cap = 0;
}

static size_t item_value_u64(item_t *item)
{
    size_t len;
    uint8_t *value = item_value(item, &len);
    uint64_t out;

    if (len != sizeof out)
        return NO_ITEM;

    memcpy(&out, value, sizeof out);
    return (size_t)out;
}

/* ------------------------------------------------------------ table ops */

static void ht_put_value(hashtable_t *t, size_t i, uint64_t value)
{
    tkey_t k = make_key(i);

    XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
    item_t **item_ref = hashtable_set(t, hash, k.bytes, k.len);
    *item_ref = make_item(k.bytes, k.len, value);
}

static void ht_put(hashtable_t *t, size_t i)
{
    ht_put_value(t, i, i);
}

/* Value stored under key i, or NO_ITEM if the key is absent. Also verifies
   the returned item actually carries the key that was asked for, which
   catches a probe that returns the wrong slot. */
static size_t ht_fetch_value(hashtable_t *t, size_t i)
{
    tkey_t k = make_key(i);
    XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
    item_t *item = hashtable_get(t, hash, k.bytes, k.len);

    if (!item)
        return NO_ITEM;

    if (!item_key_matches(item, k.bytes, k.len))
    {
        g_failures++;
        fprintf(stderr, "  FAIL in %s: get(key %zu) returned an item with a "
                        "different key\n",
                g_current_test, i);
        return NO_ITEM;
    }

    return item_value_u64(item);
}

static void ht_del(hashtable_t *t, size_t i)
{
    tkey_t k = make_key(i);

    XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
    hashtable_delete(t, hash, k.bytes, k.len);
}

/* --------------------------------------------------------------- basics */

static void test_init_rejects_non_power_of_two(void)
{
    CHECK(hashtable_init(3) == NULL);
    CHECK(hashtable_init(100) == NULL);
    CHECK(hashtable_init(4095) == NULL);

    hashtable_t *t = hashtable_init(0); /* 0 means "use the default" */
    CHECK(t != NULL);
    if (t)
    {
        CHECK(hashtable_bucket_count(t) > 0);
        hashtable_destroy(t);
    }

    t = hashtable_init(1);
    CHECK(t != NULL);
    if (t)
        hashtable_destroy(t);
}

static void test_empty_table(void)
{
    hashtable_t *t = hashtable_init(64);

    CHECK_EQ_SIZE(hashtable_len(t), 0);
    CHECK_EQ_SIZE(ht_fetch_value(t, 0), NO_ITEM);
    CHECK_EQ_SIZE(ht_fetch_value(t, 12345), NO_ITEM);

    /* Deleting from an empty table must not underflow len. */
    ht_del(t, 7);
    CHECK_EQ_SIZE(hashtable_len(t), 0);

    hashtable_destroy(t);
}

static void test_set_get_delete_roundtrip(void)
{
    hashtable_t *t = hashtable_init(64);

    ht_put(t, 42);
    CHECK_EQ_SIZE(hashtable_len(t), 1);
    CHECK_EQ_SIZE(ht_fetch_value(t, 42), 42);

    ht_del(t, 42);
    CHECK_EQ_SIZE(hashtable_len(t), 0);
    CHECK_EQ_SIZE(ht_fetch_value(t, 42), NO_ITEM);

    /* Slot must be reusable after deletion. */
    ht_put(t, 42);
    CHECK_EQ_SIZE(hashtable_len(t), 1);
    CHECK_EQ_SIZE(ht_fetch_value(t, 42), 42);

    hashtable_destroy(t);
}

static void test_overwrite_does_not_change_len(void)
{
    hashtable_t *t = hashtable_init(64);

    ht_put_value(t, 9, 1);
    ht_put_value(t, 9, 2);
    ht_put_value(t, 9, 3);

    CHECK_EQ_SIZE(hashtable_len(t), 1);
    CHECK_EQ_SIZE(ht_fetch_value(t, 9), 3);

    hashtable_destroy(t);
}

static void test_delete_absent_key(void)
{
    hashtable_t *t = hashtable_init(64);

    for (size_t i = 0; i < 50; i++)
        ht_put(t, i);
    CHECK_EQ_SIZE(hashtable_len(t), 50);

    for (size_t i = 1000; i < 1050; i++)
        ht_del(t, i);
    CHECK_EQ_SIZE(hashtable_len(t), 50);

    for (size_t i = 0; i < 50; i++)
        CHECK_EQ_SIZE(ht_fetch_value(t, i), i);

    hashtable_destroy(t);
}

static void test_no_false_positives(void)
{
    hashtable_t *t = hashtable_init(256);

    for (size_t i = 0; i < 2000; i++)
        ht_put(t, i);

    /* Keys never inserted must miss. Catches a hash-only compare that
       matches on truncated or colliding fingerprints. */
    for (size_t i = 100000; i < 110000; i++)
        CHECK_EQ_SIZE(ht_fetch_value(t, i), NO_ITEM);

    hashtable_destroy(t);
}

static void test_zero_length_key(void)
{
    hashtable_t *t = hashtable_init(64);
    uint8_t dummy = 0;

    XXH64_hash_t hash = XXH3_64bits(&dummy, 0);
    item_t **item_ref = hashtable_set(t, hash, &dummy, 0);
    *item_ref = make_item(&dummy, 0, 1);

    CHECK_EQ_SIZE(hashtable_len(t), 1);

    hash = XXH3_64bits(&dummy, 0);
    item_t *item = hashtable_get(t, hash, &dummy, 0);
    CHECK(item != NULL);
    if (item)
        CHECK_EQ_SIZE(item_value_u64(item), 1);

    hash = XXH3_64bits(&dummy, 0);
    hashtable_delete(t, hash, &dummy, 0);
    CHECK_EQ_SIZE(hashtable_len(t), 0);

    hash = XXH3_64bits(&dummy, 0);
    CHECK(hashtable_get(t, hash, &dummy, 0) == NULL);

    hashtable_destroy(t);
}

/* ------------------------------------------------------------- chaining */

/* Forces many keys into one bucket so the chain path is exercised: allocation,
   traversal, insertion into a chained bucket, and deletion from one. */
static void test_bucket_chaining(void)
{
    hashtable_t *t = hashtable_init(1024);
    const size_t n = 6000;
    for (size_t i = 0; i < n; i++)
        ht_put(t, i);

    CHECK_EQ_SIZE(hashtable_len(t), n);
    for (size_t i = 0; i < n; i++)
        CHECK_EQ_SIZE(ht_fetch_value(t, i), i);

    /* Delete every third key, then verify the survivors: exercises removal
       from chained buckets and any empty-chain reclamation. */
    size_t deleted = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        ht_del(t, i);
        deleted++;
    }

    CHECK_EQ_SIZE(hashtable_len(t), n - deleted);
    for (size_t i = 0; i < n; i++)
        CHECK_EQ_SIZE(ht_fetch_value(t, i), (i % 3 == 0) ? NO_ITEM : i);

    /* Refill: freed chain slots must be reusable. */
    for (size_t i = 0; i < n; i += 3)
        ht_put(t, i);
    CHECK_EQ_SIZE(hashtable_len(t), n);
    for (size_t i = 0; i < n; i++)
        CHECK_EQ_SIZE(ht_fetch_value(t, i), i);

    hashtable_destroy(t);
}

/* -------------------------------------------------------------- growth */

static void test_growth_preserves_every_key(void)
{
    const size_t n = 200000;
    hashtable_t *t = hashtable_init(64);
    size_t start_buckets = hashtable_bucket_count(t);

    for (size_t i = 0; i < n; i++)
    {
        ht_put(t, i);

        /* len must be exact after every single insert, not just at the end.
           A migration that drops or duplicates an entry shows up here at the
           op it happened, rather than as a mystery total. */
        if (hashtable_len(t) != i + 1)
        {
            CHECK_EQ_SIZE(hashtable_len(t), i + 1);
            break;
        }
    }

    CHECK(hashtable_bucket_count(t) > start_buckets);
    CHECK_EQ_SIZE(hashtable_len(t), n);

    for (size_t i = 0; i < n; i++)
    {
        size_t v = ht_fetch_value(t, i);
        if (v != i)
        {
            CHECK_EQ_SIZE(v, i);
            break;
        }
    }

    hashtable_destroy(t);
}

/* Reads must not be lost while the table is split across old and new. */
static void test_lookups_during_migration(void)
{
    hashtable_t *t = hashtable_init(1024);
    size_t inserted = 0;

    while (!hashtable_migrating(t) && inserted < 200000)
        ht_put(t, inserted++);

    if (!hashtable_migrating(t))
    {
        printf("    (skipped: table never reported migrating)\n");
        hashtable_destroy(t);
        return;
    }

    /* Every key inserted so far must be findable mid-migration, whether it
       sits in the migrated prefix or the untouched tail. */
    for (size_t i = 0; i < inserted; i++)
    {
        size_t v = ht_fetch_value(t, i);
        if (v != i)
        {
            CHECK_EQ_SIZE(v, i);
            break;
        }
    }

    CHECK_EQ_SIZE(hashtable_len(t), inserted);
    hashtable_destroy(t);
}

static void test_overwrite_during_migration(void)
{
    hashtable_t *t = hashtable_init(1024);
    size_t inserted = 0;

    while (!hashtable_migrating(t) && inserted < 200000)
        ht_put(t, inserted++);

    if (!hashtable_migrating(t))
    {
        printf("    (skipped: table never reported migrating)\n");
        hashtable_destroy(t);
        return;
    }

    size_t len_before = hashtable_len(t);

    /* Rewriting existing keys must not create a second copy in the new
       table while the original still lives in the old one. */
    for (size_t i = 0; i < inserted; i += 7)
        ht_put_value(t, i, i + 1000000);

    CHECK_EQ_SIZE(hashtable_len(t), len_before);

    for (size_t i = 0; i < inserted; i += 7)
    {
        size_t v = ht_fetch_value(t, i);
        if (v != i + 1000000)
        {
            CHECK_EQ_SIZE(v, i + 1000000);
            break;
        }
    }

    hashtable_destroy(t);
}

static void test_delete_during_migration(void)
{
    hashtable_t *t = hashtable_init(1024);
    size_t inserted = 0;

    while (!hashtable_migrating(t) && inserted < 200000)
        ht_put(t, inserted++);

    if (!hashtable_migrating(t))
    {
        printf("    (skipped: table never reported migrating)\n");
        hashtable_destroy(t);
        return;
    }

    size_t deleted = 0;
    for (size_t i = 0; i < inserted; i += 5)
    {
        ht_del(t, i);
        deleted++;
    }

    /* A delete that only reaches one of the two tables leaves the entry
       alive in the other; it would reappear once migration completes. */
    CHECK_EQ_SIZE(hashtable_len(t), inserted - deleted);

    for (size_t i = 0; i < inserted; i += 5)
        if (ht_fetch_value(t, i) != NO_ITEM)
        {
            CHECK_EQ_SIZE(ht_fetch_value(t, i), NO_ITEM);
            break;
        }

    /* Drive migration to completion, then re-verify: resurrection shows up
       only after the old table is dropped. */
    for (size_t i = inserted; i < inserted + 200000 && hashtable_migrating(t); i++)
        ht_put(t, i);

    for (size_t i = 0; i < inserted; i += 5)
        if (ht_fetch_value(t, i) != NO_ITEM)
        {
            CHECK_EQ_SIZE(ht_fetch_value(t, i), NO_ITEM);
            fprintf(stderr, "  (key %zu resurrected after migration completed)\n", i);
            break;
        }

    hashtable_destroy(t);
}

static void test_repeated_fill_and_drain(void)
{
    hashtable_t *t = hashtable_init(128);

    /* Growth is one-way, so a table that fills and empties repeatedly must
       stay correct at capacities far above its live set. */
    for (size_t round = 0; round < 4; round++)
    {
        const size_t n = 20000;

        for (size_t i = 0; i < n; i++)
            ht_put(t, i + round * 100000);
        CHECK_EQ_SIZE(hashtable_len(t), n);

        for (size_t i = 0; i < n; i++)
            ht_del(t, i + round * 100000);
        CHECK_EQ_SIZE(hashtable_len(t), 0);
    }

    hashtable_destroy(t);
}

/* ---------------------------------------------------- differential test */

/* Independent reference map: open addressing with tombstones, FNV-1a. Shares
   no code with the table under test, so the two are unlikely to agree on a
   wrong answer. Stores the same item pointer the table was handed, so the
   comparison below is pointer identity: the table must return the exact
   item most recently set, not merely one with an equal value. */
typedef struct
{
    uint8_t *key;
    size_t len;
    item_t *val;
    uint8_t state;
} ref_slot_t;
typedef struct
{
    size_t cap, len, used;
    ref_slot_t *s;
} ref_t;

static uint64_t ref_hash(const uint8_t *k, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++)
    {
        h ^= k[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void ref_init(ref_t *r, size_t cap)
{
    r->cap = cap;
    r->len = 0;
    r->used = 0;
    r->s = calloc(cap, sizeof(ref_slot_t));
}

static void ref_free(ref_t *r)
{
    for (size_t i = 0; i < r->cap; i++)
        if (r->s[i].state == 1)
            free(r->s[i].key);
    free(r->s);
}

static void ref_grow(ref_t *r);

static void ref_set(ref_t *r, const uint8_t *k, size_t n, item_t *v)
{
    if ((r->used + 1) * 2 > r->cap)
        ref_grow(r);

    size_t i = ref_hash(k, n) & (r->cap - 1);
    long tomb = -1;

    for (;;)
    {
        ref_slot_t *s = &r->s[i];
        if (s->state == 0)
        {
            if (tomb >= 0)
                s = &r->s[tomb];
            else
                r->used++;
            s->key = malloc(n ? n : 1);
            memcpy(s->key, k, n);
            s->len = n;
            s->val = v;
            s->state = 1;
            r->len++;
            return;
        }
        if (s->state == 2)
        {
            if (tomb < 0)
                tomb = (long)i;
        }
        else if (s->len == n && memcmp(s->key, k, n) == 0)
        {
            s->val = v;
            return;
        }
        i = (i + 1) & (r->cap - 1);
    }
}

static void ref_grow(ref_t *r)
{
    ref_t bigger;
    ref_init(&bigger, r->cap * 2);
    for (size_t i = 0; i < r->cap; i++)
        if (r->s[i].state == 1)
            ref_set(&bigger, r->s[i].key, r->s[i].len, r->s[i].val);
    ref_free(r);
    *r = bigger;
}

static item_t *ref_get(ref_t *r, const uint8_t *k, size_t n)
{
    size_t i = ref_hash(k, n) & (r->cap - 1);
    for (;;)
    {
        ref_slot_t *s = &r->s[i];
        if (s->state == 0)
            return NULL;
        if (s->state == 1 && s->len == n && memcmp(s->key, k, n) == 0)
            return s->val;
        i = (i + 1) & (r->cap - 1);
    }
}

static void ref_del(ref_t *r, const uint8_t *k, size_t n)
{
    size_t i = ref_hash(k, n) & (r->cap - 1);
    for (;;)
    {
        ref_slot_t *s = &r->s[i];
        if (s->state == 0)
            return;
        if (s->state == 1 && s->len == n && memcmp(s->key, k, n) == 0)
        {
            free(s->key);
            s->key = NULL;
            s->state = 2;
            r->len--;
            return;
        }
        i = (i + 1) & (r->cap - 1);
    }
}

/* Random op mix against the reference. Key space is deliberately small
   relative to op count so overwrites, deletes and reinserts collide often. */
static void differential_stress(uint64_t seed, size_t ops, size_t key_space)
{
    hashtable_t *t = hashtable_init(64);
    ref_t r;
    ref_init(&r, 1024);

    uint64_t state = seed;
    size_t mismatches = 0;

    for (size_t op = 0; op < ops && mismatches < 5; op++)
    {
        uint64_t roll = splitmix64(&state);
        size_t i = (size_t)(splitmix64(&state) % key_space);
        tkey_t k = make_key(i);

        switch (roll % 10)
        {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5: /* 60% set */
        {
            item_t *item = make_item(k.bytes, k.len, i);
            XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
            item_t **item_ref = hashtable_set(t, hash, k.bytes, k.len);
            *item_ref = item;

            ref_set(&r, k.bytes, k.len, item);
            break;
        }
        case 6:
        case 7:
        case 8: /* 30% get */
        {
            XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
            item_t *a = hashtable_get(t, hash, k.bytes, k.len);
            item_t *b = ref_get(&r, k.bytes, k.len);
            if (a != b)
            {
                mismatches++;
                fprintf(stderr,
                        "  op %zu: get(key %zu) = %p, reference = %p\n",
                        op, i, (void *)a, (void *)b);
            }
            break;
        }
        default: /* 10% delete */
        {
            XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
            hashtable_delete(t, hash, k.bytes, k.len);
            ref_del(&r, k.bytes, k.len);
            break;
        }
        }

        if (hashtable_len(t) != r.len)
        {
            mismatches++;
            fprintf(stderr, "  op %zu: len = %zu, reference = %zu\n",
                    op, hashtable_len(t), r.len);
        }
    }

    CHECK_EQ_SIZE(mismatches, 0);

    /* Final full sweep over the whole key space, present and absent alike. */
    size_t sweep_bad = 0;
    for (size_t i = 0; i < key_space && sweep_bad < 5; i++)
    {
        tkey_t k = make_key(i);
        XXH64_hash_t hash = XXH3_64bits(k.bytes, k.len);
        item_t *a = hashtable_get(t, hash, k.bytes, k.len);
        item_t *b = ref_get(&r, k.bytes, k.len);
        if (a != b)
        {
            sweep_bad++;
            fprintf(stderr, "  sweep: key %zu = %p, reference = %p\n",
                    i, (void *)a, (void *)b);
        }
    }
    CHECK_EQ_SIZE(sweep_bad, 0);

    ref_free(&r);
    hashtable_destroy(t);
}

static void test_differential_small_keyspace(void)
{
    differential_stress(1, 200000, 500);
}
static void test_differential_wide_keyspace(void)
{
    differential_stress(2, 200000, 50000);
}
static void test_differential_delete_heavy(void)
{
    differential_stress(3, 300000, 2000);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    RUN(test_init_rejects_non_power_of_two);
    RUN(test_empty_table);
    RUN(test_set_get_delete_roundtrip);
    RUN(test_overwrite_does_not_change_len);
    RUN(test_delete_absent_key);
    RUN(test_no_false_positives);
    RUN(test_zero_length_key);
    RUN(test_bucket_chaining);
    RUN(test_growth_preserves_every_key);
    RUN(test_lookups_during_migration);
    RUN(test_overwrite_during_migration);
    RUN(test_delete_during_migration);
    RUN(test_repeated_fill_and_drain);
    RUN(test_differential_small_keyspace);
    RUN(test_differential_wide_keyspace);
    RUN(test_differential_delete_heavy);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}