#define _GNU_SOURCE

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "hashtable.h"

#define DEFAULT_BUCKET_COUNT 262144UL
#define DEFAULT_ITEMS_COUNT 1000000UL
#define DEFAULT_KEY_LEN 24UL
#define DEFAULT_REPS 5UL

/* Ops per timing window during the insert phase. Small enough to isolate a
   single migration step, large enough that clock_gettime is noise. */
#define WINDOW_OPS 4096UL
#define WORST_WINDOWS 5

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

/* Stored value for key i is its successor index, biased by 1 so it is never
   NULL. Lets a lookup's result feed the next lookup's key with no arithmetic. */
static inline void *value_for(const size_t *next, size_t i)
{
    return (void *)(uintptr_t)(next[i] + 1);
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
    printf("%-10s %8.2f ns/op | %7.2f M ops/s\n",
           label, ns_per_op, 1e3 / ns_per_op);
}

/* ---------------------------------------------------------------- phases */

/* Inserts every key, recording throughput per window so growth stalls stay
   visible instead of being averaged away. */
static double bench_set(const bench_config_t *cfg, const keyset_t *ks,
                        const size_t *next, window_t *windows, size_t *window_count)
{
    double best_ns_per_op = 0.0;
    size_t n_windows = (cfg->items_count + WINDOW_OPS - 1) / WINDOW_OPS;
    window_t *scratch = xmalloc(n_windows * sizeof(window_t));

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        hashtable_t *table = hashtable_init(cfg->bucket_count);

        if (!table)
        {
            fprintf(stderr, "hashtable_init failed (bucket_count power of two?)\n");
            exit(EXIT_FAILURE);
        }

        size_t w = 0;
        uint64_t total = 0;
        uint64_t window_start = now_ns();

        COMPILER_BARRIER();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            hashtable_set(table, key_at(ks, i), ks->key_len, value_for(next, i));

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
                window_start = now;
                w++;
            }
        }

        double ns_per_op = (double)total / (double)cfg->items_count;

        if (rep == 0 || ns_per_op < best_ns_per_op)
        {
            best_ns_per_op = ns_per_op;
            memcpy(windows, scratch, w * sizeof(window_t));
            *window_count = w;
        }

        hashtable_destroy(table);
    }

    free(scratch);
    return best_ns_per_op;
}

/* Fills a table once; the caller times lookups against it. */
static hashtable_t *build_table(const bench_config_t *cfg, const keyset_t *ks,
                                const size_t *next)
{
    hashtable_t *table = hashtable_init(cfg->bucket_count);

    if (!table)
    {
        fprintf(stderr, "hashtable_init failed (bucket_count power of two?)\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < cfg->items_count; i++)
        hashtable_set(table, key_at(ks, i), ks->key_len, value_for(next, i));

    return table;
}

/* Independent lookups: the CPU overlaps ~10 of these, so this is throughput,
   not per-lookup latency. Verifies every key is still findable.

   Keys are walked sequentially on purpose. Bucket index is hash(key) & mask,
   so insertion order already yields random bucket order — shuffling the key
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
            void *v = hashtable_get(table, key_at(ks, i), ks->key_len);

            checksum += (uintptr_t)v;
            misses += (v == NULL);
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(checksum);

        if (misses)
            fprintf(stderr,
                    "ERROR: %zu of %zu inserted keys not found — table lost entries\n",
                    misses, cfg->items_count);

        double ns_per_op = (double)(end - start) / (double)cfg->items_count;

        if (rep == 0 || ns_per_op < best)
            best = ns_per_op;
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
            void *v = hashtable_get(table, key_at(absent, i), absent->key_len);

            hits += (v != NULL);
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(hits);

        if (hits)
            fprintf(stderr, "ERROR: %" PRIu64 " false positives on absent keys\n", hits);

        double ns_per_op = (double)(end - start) / (double)cfg->items_count;

        if (rep == 0 || ns_per_op < best)
            best = ns_per_op;
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
        size_t idx = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
        {
            void *v = hashtable_get(table, key_at(ks, idx), ks->key_len);

            if (!v)
                break;

            idx = (uintptr_t)v - 1;
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64((uint64_t)idx);

        double ns_per_op = (double)(end - start) / (double)cfg->items_count;

        if (rep == 0 || ns_per_op < best)
            best = ns_per_op;
    }

    return best;
}

static double bench_delete(const bench_config_t *cfg, const keyset_t *ks,
                           const size_t *next)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        hashtable_t *table = build_table(cfg, ks, next);

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < cfg->items_count; i++)
            hashtable_delete(table, key_at(ks, i), ks->key_len);

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        double ns_per_op = (double)(end - start) / (double)cfg->items_count;

        if (rep == 0 || ns_per_op < best)
            best = ns_per_op;

        hashtable_destroy(table);
    }

    return best;
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

    fprintf(f, "start_op,ns_per_op\n");

    for (size_t i = 0; i < count; i++)
        fprintf(f, "%zu,%.3f\n", windows[i].start_op, windows[i].ns_per_op);

    fclose(f);
    printf("wrote per-window timings to %s\n", path);
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

    printf("buckets=%zu items=%zu key_len=%zu reps=%zu\n"
           "initial slots=%zu, initial load=%.2f%% (table may grow past this)\n\n",
           cfg.bucket_count, cfg.items_count, cfg.key_len, cfg.reps,
           cfg.bucket_count * 8,
           100.0 * (double)cfg.items_count / (double)(cfg.bucket_count * 8));

    size_t window_count = 0;
    window_t *windows = xmalloc(((cfg.items_count + WINDOW_OPS - 1) / WINDOW_OPS) *
                                sizeof(window_t));

    report("SET", bench_set(&cfg, &keys, next, windows, &window_count));

    /* Sorted copy: the worst windows are where growth stalled the insert. */
    window_t *sorted = xmalloc(window_count * sizeof(window_t));
    memcpy(sorted, windows, window_count * sizeof(window_t));
    qsort(sorted, window_count, sizeof(window_t), window_cmp_desc);

    printf("           worst %d of %zu windows (%lu ops each):\n",
           WORST_WINDOWS, window_count, WINDOW_OPS);

    for (size_t i = 0; i < WORST_WINDOWS && i < window_count; i++)
        printf("             op %8zu  %9.2f ns/op  buckets=%-8zu %s\n",
               sorted[i].start_op, sorted[i].ns_per_op,
               sorted[i].bucket_count,
               sorted[i].migrating ? "MIGRATING" : "quiescent");

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
        printf("           migrating: %6.2f ns/op over %zu windows (%.0f%% of inserts)\n",
               mig_sum / (double)mig_n, mig_n,
               100.0 * (double)mig_n / (double)window_count);
    if (quiet_n)
        printf("           quiescent: %6.2f ns/op over %zu windows\n",
               quiet_sum / (double)quiet_n, quiet_n);

    printf("           growth: ");
    size_t prev = 0;
    for (size_t i = 0; i < window_count; i++)
        if (windows[i].bucket_count != prev)
        {
            printf("%zu@op%zu ", windows[i].bucket_count, windows[i].start_op);
            prev = windows[i].bucket_count;
        }
    printf("\n");

    const char *csv = getenv("BENCH_WINDOWS_CSV");
    if (csv)
        dump_windows(csv, windows, window_count);

    hashtable_t *table = build_table(&cfg, &keys, next);

    report("GET hit", bench_get_hit(&cfg, table, &keys));
    report("GET miss", bench_get_miss(&cfg, table, &absent));
    report("GET chain", bench_get_chain(&cfg, table, &keys));

    hashtable_destroy(table);

    report("DELETE", bench_delete(&cfg, &keys, next));

    free(sorted);
    free(windows);
    free(next);
    free_keyset(&absent);
    free_keyset(&keys);

    return EXIT_SUCCESS;
}