#define _GNU_SOURCE

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "hashtable.h"
#include "item.h"
#include "xxhash.h"

#define DEFAULT_BUCKET_COUNT 262144UL
#define DEFAULT_ITEMS_COUNT 1000000UL
#define DEFAULT_KEY_LEN 24UL
#define DEFAULT_REPS 5UL

#define HUGE_PAGE (2UL * 1024 * 1024)

/* Ops per timing window during the insert phase. Small enough to isolate a
   single migration step, large enough that clock_gettime is noise. */
#define WINDOW_OPS 4096UL
#define WORST_WINDOWS 5

/* Working set for the DRAM reference: well past any L3. */
#define CHASE_BYTES ((1UL << 20) * 84)
#define CHASE_STEPS 5000000UL

typedef struct bench_config
{
    size_t bucket_count;
    size_t items_count;
    size_t key_len;
    size_t reps;
} bench_config_t;

/* ------------------------------------------------------------------ util */

/* Stops the compiler hoisting loop bodies across the timer reads. */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* Consumes a value so the optimizer cannot delete the work that produced it. */
static inline void sink_u64(uint64_t value)
{
    __asm__ __volatile__("" ::"r"(value) : "memory");
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        abort();

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

    return z ^ (z >> 31);
}

static void *xmalloc(size_t bytes)
{
    void *p = malloc(bytes);

    if (!p)
    {
        fprintf(stderr, "out of memory (%zu bytes)\n", bytes);
        exit(EXIT_FAILURE);
    }

    return p;
}

/* Anonymous mapping rounded up to 2 MiB, THP requested. Pages are zeroed and
   must be released with munmap(p, *mapped_out), never free(). */
static void *alloc_huge(size_t bytes, size_t *mapped_out)
{
    size_t mapped = (bytes + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1);
    void *p = mmap(NULL, mapped, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED)
        return NULL;

    /* Best effort: ignored when THP is off, and the mapping still works. */
    madvise(p, mapped, MADV_HUGEPAGE);

    *mapped_out = mapped;
    return p;
}

static void pin_to_cpu(int cpu)
{
#ifdef __linux__
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
#else
    (void)cpu;
#endif
}

/* ------------------------------------------------------------------ keys */

typedef struct keyset
{
    uint8_t *data;
    size_t count;
    size_t key_len;
} keyset_t;

static inline const uint8_t *key_at(const keyset_t *ks, size_t i)
{
    return ks->data + i * ks->key_len;
}

static keyset_t make_keyset(size_t count, size_t key_len, uint64_t seed)
{
    keyset_t ks = {xmalloc(count * key_len), count, key_len};
    uint64_t state = seed;

    for (size_t i = 0; i < count; i++)
    {
        uint8_t *key = ks.data + i * key_len;

        for (size_t off = 0; off < key_len; off += 8)
        {
            uint64_t r = splitmix64(&state);
            size_t chunk = (key_len - off < 8) ? key_len - off : 8;

            memcpy(key + off, &r, chunk);
        }
    }

    return ks;
}

static void free_keyset(keyset_t *ks)
{
    free(ks->data);
    ks->data = NULL;
}

/* Sattolo: permutation guaranteed to be a single n-cycle, so following
   next[] visits every key exactly once before repeating. Used to build a
   dependent chain for the latency measurement. */
static size_t *make_cycle(size_t n, uint64_t seed)
{
    size_t *next = xmalloc(n * sizeof(size_t));
    uint64_t state = seed;

    for (size_t i = 0; i < n; i++)
        next[i] = i;

    for (size_t i = n - 1; i > 0; i--)
    {
        size_t j = (size_t)(splitmix64(&state) % i); /* strictly j < i */
        size_t tmp = next[i];

        next[i] = next[j];
        next[j] = tmp;
    }

    return next;
}

/* ----------------------------------------------------------------- items */

/* item_t is a flexible-array struct with no constructor, so the harness owns
   item storage. Every key is the same length and every value is the key of
   the chain successor, so the whole set is one mmap'd arena with a fixed
   stride: no per-item malloc, no pointer table, items in insertion order.

   Ownership: the table stores these pointers and must not free them.
   hashtable_destroy releases buckets only. The arena outlives every table
   the benchmark builds. */

#define ITEM_ALIGN 8 /* >= alignof(item_t); keeps every stride step aligned */

_Static_assert(ITEM_ALIGN % _Alignof(item_t) == 0, "ITEM_ALIGN below item_t alignment");

typedef struct itemset
{
    uint8_t *arena;
    size_t stride;
    size_t mapped;
    size_t count;
} itemset_t;

static inline item_t *item_at(const itemset_t *is, size_t i)
{
    return (item_t *)(is->arena + i * is->stride);
}

static itemset_t make_itemset(const keyset_t *ks, const size_t *next)
{
    size_t data_len = ks->key_len + ks->key_len; /* key + successor key */
    size_t raw = sizeof(item_t) + data_len;
    size_t stride = (raw + ITEM_ALIGN - 1) & ~(size_t)(ITEM_ALIGN - 1);
    itemset_t is = {
        .arena = NULL,
        .mapped = 0,
        .stride = stride,
        .count = ks->count,
    };

    if (ks->key_len > UINT16_MAX)
    {
        fprintf(stderr, "key_len %zu exceeds item_t key_len field\n", ks->key_len);
        exit(EXIT_FAILURE);
    }

    is.arena = alloc_huge(stride * ks->count, &is.mapped);

    if (!is.arena)
    {
        fprintf(stderr, "mmap failed (%zu items, %zu bytes each)\n",
                ks->count, stride);
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < ks->count; i++)
    {
        item_t *item = item_at(&is, i);

        /* Not from a slab: no owner, and data_size is what the stride holds.
           Lengths are set by item_set_key / item_set_value. */
        item->slab_ptr = NULL;
        item->data_size = (uint32_t)(stride - sizeof(item_t));
        item->flags = 0;
        item->expire_at = 0;

        item_set_key(item, key_at(ks, i), ks->key_len);
        item_set_value(item, key_at(ks, next[i]), ks->key_len);
    }

    return is;
}

static void free_itemset(itemset_t *is)
{
    munmap(is->arena, is->mapped);
    is->arena = NULL;
    is->mapped = 0;
}

/* --------------------------------------------------------------- results */

typedef struct window
{
    size_t start_op;
    double ns_per_op;
    size_t bucket_count;
    bool migrating;
} window_t;

static int window_cmp_desc(const void *a, const void *b)
{
    double x = ((const window_t *)a)->ns_per_op;
    double y = ((const window_t *)b)->ns_per_op;

    return (x < y) - (x > y);
}

static void report(const char *label, double ns_per_op)
{
    printf("%-14s %8.2f ns/op | %7.2f M ops/s\n",
           label, ns_per_op, 1e3 / ns_per_op);
}

static inline void keep_min(double *best, size_t rep, double v)
{
    if (rep == 0 || v < *best)
        *best = v;
}

static hashtable_t *new_table(const bench_config_t *cfg)
{
    hashtable_t *table = hashtable_init(cfg->bucket_count);

    if (!table)
    {
        fprintf(stderr, "hashtable_init failed (bucket_count power of two?)\n");
        exit(EXIT_FAILURE);
    }

    return table;
}

static inline void insert(hashtable_t *table, const keyset_t *ks,
                          const itemset_t *items, size_t i)
{
    XXH64_hash_t hash = XXH3_64bits(key_at(ks, i), ks->key_len);
    item_t **slot = hashtable_set(table, hash, key_at(ks, i), ks->key_len);

    if (!slot)
    {
        fprintf(stderr, "hashtable_set failed at op %zu\n", i);
        exit(EXIT_FAILURE);
    }

    *slot = item_at(items, i);
}

/* ---------------------------------------------------------------- phases */

/* Inserts every key, recording throughput per window so growth stalls stay
   visible instead of being averaged away. Window bookkeeping (including the
   table introspection calls) sits between two clock reads and is excluded
   from both the window and the total. */
static double bench_set(const bench_config_t *cfg, const keyset_t *ks,
                        const itemset_t *items, window_t *windows,
                        size_t *window_count)
{
    double best = 0.0;
    size_t n_windows = (cfg->items_count + WINDOW_OPS - 1) / WINDOW_OPS;
    window_t *scratch = xmalloc(n_windows * sizeof(window_t));

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        hashtable_t *table = new_table(cfg);
        size_t w = 0;
        uint64_t total = 0;

        COMPILER_BARRIER();
        uint64_t window_start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            insert(table, ks, items, i);

            if ((i + 1) % WINDOW_OPS == 0 || i + 1 == cfg->items_count)
            {
                uint64_t now = now_ns();
                size_t ops = (i % WINDOW_OPS) + 1;

                COMPILER_BARRIER();

                scratch[w].start_op = i + 1 - ops;
                scratch[w].ns_per_op = (double)(now - window_start) / (double)ops;
                scratch[w].bucket_count = hashtable_bucket_count(table);
                scratch[w].migrating = hashtable_migrating(table);
                total += now - window_start;
                w++;

                COMPILER_BARRIER();
                window_start = now_ns();
            }
        }

        double ns_per_op = (double)total / (double)cfg->items_count;

        if (rep == 0 || ns_per_op < best)
        {
            best = ns_per_op;
            memcpy(windows, scratch, w * sizeof(window_t));
            *window_count = w;
        }

        hashtable_destroy(table);
    }

    free(scratch);
    return best;
}

/* Fills a table once; the caller times operations against it. */
static hashtable_t *build_table(const bench_config_t *cfg, const keyset_t *ks,
                                const itemset_t *items)
{
    hashtable_t *table = new_table(cfg);

    for (size_t i = 0; i < cfg->items_count; i++)
        insert(table, ks, items, i);

    return table;
}

/* Independent lookups: the CPU overlaps ~10 of these, so this is throughput,
   not per-lookup latency. Verifies every key is still findable.

   Keys are walked sequentially on purpose. Bucket index comes from the hash,
   so insertion order already yields random bucket order; shuffling the key
   index only adds a random miss on the key array itself, which is harness
   cost, not table cost. */
static double bench_get_hit(const bench_config_t *cfg, hashtable_t *table,
                            const keyset_t *ks)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        uint64_t checksum = 0;
        size_t misses = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            XXH64_hash_t hash = XXH3_64bits(key_at(ks, i), ks->key_len);
            item_t *item = hashtable_get(table, hash, key_at(ks, i), ks->key_len);

            checksum += (uintptr_t)item;
            misses += (item == NULL);
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(checksum);

        if (misses)
            fprintf(stderr,
                    "ERROR: %zu of %zu inserted keys not found (table lost entries)\n",
                    misses, cfg->items_count);

        keep_min(&best, rep, (double)(end - start) / (double)cfg->items_count);
    }

    return best;
}

/* Misses walk the full bucket plus any chain, so this is the worst-case probe
   and the one a cache in front of a backing store hits constantly. */
static double bench_get_miss(const bench_config_t *cfg, hashtable_t *table,
                             const keyset_t *absent)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        uint64_t hits = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            XXH64_hash_t hash = XXH3_64bits(key_at(absent, i), absent->key_len);
            item_t *item = hashtable_get(table, hash, key_at(absent, i), absent->key_len);

            hits += (item != NULL);
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(hits);

        if (hits)
            fprintf(stderr, "ERROR: %" PRIu64 " false positives on absent keys\n", hits);

        keep_min(&best, rep, (double)(end - start) / (double)cfg->items_count);
    }

    return best;
}

/* Each lookup's key comes from the previous lookup's value, so nothing
   overlaps. This is the number that matters for request tail latency. */
static double bench_get_chain(const bench_config_t *cfg, hashtable_t *table,
                              const keyset_t *ks)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        const uint8_t *key = key_at(ks, 0);
        size_t steps = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            size_t len;
            XXH64_hash_t hash = XXH3_64bits(key, ks->key_len);
            item_t *item = hashtable_get(table, hash, key, ks->key_len);

            if (!item)
                break;

            key = item_value(item, &len);
            steps++;
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64((uint64_t)(uintptr_t)key);

        if (steps != cfg->items_count)
        {
            fprintf(stderr, "ERROR: chain broke after %zu of %zu steps\n",
                    steps, cfg->items_count);
            if (steps == 0)
                break;
        }

        keep_min(&best, rep, (double)(end - start) / (double)steps);
    }

    return best;
}

/* Every delete must return the exact item inserted for that key. */
static double bench_delete(const bench_config_t *cfg, const keyset_t *ks,
                           const itemset_t *items)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        hashtable_t *table = build_table(cfg, ks, items);
        size_t wrong = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            XXH64_hash_t hash = XXH3_64bits(key_at(ks, i), ks->key_len);
            item_t *item = hashtable_delete(table, hash, key_at(ks, i), ks->key_len);

            wrong += (item != item_at(items, i));
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        if (wrong)
            fprintf(stderr, "ERROR: %zu deletes missed or returned the wrong item\n", wrong);

        keep_min(&best, rep, (double)(end - start) / (double)cfg->items_count);

        hashtable_destroy(table);
    }

    return best;
}

/* DRAM reference: one dependent random load per step over a buffer far
   larger than L3, one cache line per slot. GET chain should land within a
   small multiple of this; how many of these it costs is the table's real
   price. Same page size as the table (no THP request), so TLB cost matches. */
static double bench_pointer_chase(size_t bytes)
{
    size_t n = bytes / 64;
    size_t *cycle = make_cycle(n, 0x5EED0004);
    uint64_t *buf = xmalloc(n * 64);

    for (size_t i = 0; i < n; i++)
        buf[i * 8] = cycle[i];

    free(cycle);

    size_t idx = 0;

    COMPILER_BARRIER();
    uint64_t start = now_ns();

    for (size_t i = 0; i < CHASE_STEPS; i++)
        idx = buf[idx * 8];

    uint64_t end = now_ns();
    COMPILER_BARRIER();
    sink_u64(idx);

    free(buf);
    return (double)(end - start) / (double)CHASE_STEPS;
}

/* ------------------------------------------------------------------ main */

static size_t parse_size(const char *arg, const char *what)
{
    char *end = NULL;
    unsigned long long value = strtoull(arg, &end, 10);

    if (*arg == '\0' || *end != '\0' || value == 0)
    {
        fprintf(stderr, "invalid %s: %s\n", what, arg);
        exit(EXIT_FAILURE);
    }

    return (size_t)value;
}

static void dump_windows(const char *path, const window_t *windows, size_t count)
{
    FILE *f = fopen(path, "w");

    if (!f)
    {
        fprintf(stderr, "warning: could not open %s\n", path);
        return;
    }

    fprintf(f, "start_op,ns_per_op,bucket_count,migrating\n");

    for (size_t i = 0; i < count; i++)
        fprintf(f, "%zu,%.3f,%zu,%d\n", windows[i].start_op, windows[i].ns_per_op,
                windows[i].bucket_count, windows[i].migrating);

    fclose(f);
    printf("wrote per-window timings to %s\n", path);
}

static void report_windows(const window_t *windows, size_t window_count)
{
    /* Sorted copy: the worst windows are where growth stalled the insert. */
    window_t *sorted = xmalloc(window_count * sizeof(window_t));

    memcpy(sorted, windows, window_count * sizeof(window_t));
    qsort(sorted, window_count, sizeof(window_t), window_cmp_desc);

    printf("               worst %d of %zu windows (%lu ops each):\n",
           WORST_WINDOWS, window_count, WINDOW_OPS);

    for (size_t i = 0; i < WORST_WINDOWS && i < window_count; i++)
        printf("                 op %8zu  %9.2f ns/op  buckets=%-8zu %s\n",
               sorted[i].start_op, sorted[i].ns_per_op, sorted[i].bucket_count,
               sorted[i].migrating ? "MIGRATING" : "quiescent");

    free(sorted);

    /* Split the average by phase: if migration is the cost, these diverge. */
    double mig_sum = 0.0, quiet_sum = 0.0;
    size_t mig_n = 0, quiet_n = 0;

    for (size_t i = 0; i < window_count; i++)
    {
        if (windows[i].migrating)
        {
            mig_sum += windows[i].ns_per_op;
            mig_n++;
        }
        else
        {
            quiet_sum += windows[i].ns_per_op;
            quiet_n++;
        }
    }

    if (mig_n)
        printf("               migrating: %6.2f ns/op over %zu windows (%.0f%% of inserts)\n",
               mig_sum / (double)mig_n, mig_n,
               100.0 * (double)mig_n / (double)window_count);
    if (quiet_n)
        printf("               quiescent: %6.2f ns/op over %zu windows\n",
               quiet_sum / (double)quiet_n, quiet_n);

    printf("               growth: ");
    size_t prev = 0;
    for (size_t i = 0; i < window_count; i++)
        if (windows[i].bucket_count != prev)
        {
            printf("%zu@op%zu ", windows[i].bucket_count, windows[i].start_op);
            prev = windows[i].bucket_count;
        }
    printf("\n");
}

int main(int argc, char **argv)
{
    bench_config_t cfg = {
        DEFAULT_BUCKET_COUNT,
        DEFAULT_ITEMS_COUNT,
        DEFAULT_KEY_LEN,
        DEFAULT_REPS,
    };

    if (argc > 5)
    {
        fprintf(stderr,
                "usage: %s [bucket_count] [items_count] [key_len] [reps]\n"
                "  BENCH_WINDOWS_CSV=path  dump per-window insert timings\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2)
        cfg.bucket_count = parse_size(argv[1], "bucket_count");
    if (argc >= 3)
        cfg.items_count = parse_size(argv[2], "items_count");
    if (argc >= 4)
        cfg.key_len = parse_size(argv[3], "key_len");
    if (argc >= 5)
        cfg.reps = parse_size(argv[4], "reps");

    pin_to_cpu(0);

    keyset_t keys = make_keyset(cfg.items_count, cfg.key_len, 0x5EED0001);
    keyset_t absent = make_keyset(cfg.items_count, cfg.key_len, 0x5EED0002);
    size_t *next = make_cycle(cfg.items_count, 0x5EED0003);
    itemset_t items = make_itemset(&keys, next);

    free(next); /* baked into the item values */

    printf("buckets=%zu items=%zu key_len=%zu reps=%zu\n"
           "initial slots=%zu, initial load=%.2f%% (table may grow past this)\n"
           "item stride=%zu bytes, arena=%.1f MiB\n\n",
           cfg.bucket_count, cfg.items_count, cfg.key_len, cfg.reps,
           cfg.bucket_count * 8,
           100.0 * (double)cfg.items_count / (double)(cfg.bucket_count * 8),
           items.stride,
           (double)(items.stride * items.count) / (1024.0 * 1024.0));

    size_t window_count = 0;
    window_t *windows = xmalloc(((cfg.items_count + WINDOW_OPS - 1) / WINDOW_OPS) *
                                sizeof(window_t));

    report("SET", bench_set(&cfg, &keys, &items, windows, &window_count));
    report_windows(windows, window_count);

    const char *csv = getenv("BENCH_WINDOWS_CSV");
    if (csv)
        dump_windows(csv, windows, window_count);

    hashtable_t *table = build_table(&cfg, &keys, &items);

    report("GET hit", bench_get_hit(&cfg, table, &keys));
    report("GET miss", bench_get_miss(&cfg, table, &absent));
    report("GET chain", bench_get_chain(&cfg, table, &keys));

    hashtable_destroy(table);

    report("DELETE", bench_delete(&cfg, &keys, &items));
    report("POINTER CHASE", bench_pointer_chase(CHASE_BYTES));

    free(windows);
    free_itemset(&items);
    free_keyset(&absent);
    free_keyset(&keys);

    return EXIT_SUCCESS;
}