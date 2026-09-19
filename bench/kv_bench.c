/*
 * kv_bench.c - end-to-end benchmark for kv_t through the public API.
 *
 * The hashtable and slab benches measure components. This one measures what
 * a request pays: hash + shard select + mutex + probe + slab alloc/free, and
 * how that holds up under skew and across threads.
 *
 *   make bench BENCH=kv_bench
 *   build/bench/bin/bench/kv_bench [items] [key_len] [value_len] [reps] [max_threads]
 */

#define _GNU_SOURCE

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "kv.h"

#define DEFAULT_ITEMS 1000000UL
#define DEFAULT_KEY_LEN 24UL
#define DEFAULT_VALUE_LEN 32UL
#define DEFAULT_REPS 5UL

#define STREAM_OPS 4000000UL /* ops per mixed stream; every MT thread runs all of it */
#define ZIPF_S 0.99          /* YCSB default skew                                     */
#define GROW_FACTOR 4        /* SET grow rewrites every value this much larger        */

/* Round-tripped through every SET so verify() can check them. */
#define TAG_FLAGS 0xA5A5u
#define TAG_EXP 0xDEADBEEFu

/* Precomputed op stream: low 31 bits key index, top bit SET vs GET. */
#define OP_SET 0x80000000u
#define OP_IDX 0x7FFFFFFFu

typedef struct bench_config
{
    size_t items;
    size_t key_len;
    size_t value_len;
    size_t reps;
    size_t max_threads;
} bench_config_t;

/* ------------------------------------------------------------------ util */

#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

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

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long total, res;

    if (!f)
        return -1;
    if (fscanf(f, "%ld %ld", &total, &res) != 2)
        res = -1;
    fclose(f);

    return res < 0 ? -1 : res * (sysconf(_SC_PAGESIZE) / 1024);
}

static inline void keep_min(double *best, size_t rep, double v)
{
    if (rep == 0 || v < *best)
        *best = v;
}

static void report(const char *label, double ns_per_op)
{
    printf("%-14s %8.2f ns/op | %7.2f M ops/s\n", label, ns_per_op, 1e3 / ns_per_op);
}

/* ------------------------------------------------------------------ rows */

/* Fixed-stride byte rows: keys and values, one flat buffer each. */
typedef struct rows
{
    uint8_t *data;
    size_t count;
    size_t len;
} rows_t;

static inline const uint8_t *row_at(const rows_t *r, size_t i)
{
    return r->data + i * r->len;
}

static rows_t make_keys(size_t count, size_t len, uint64_t seed)
{
    rows_t ks = {xmalloc(count * len), count, len};
    uint64_t state = seed;

    for (size_t i = 0; i < count; i++)
    {
        uint8_t *key = ks.data + i * len;

        for (size_t off = 0; off < len; off += 8)
        {
            uint64_t r = splitmix64(&state);
            size_t chunk = (len - off < 8) ? len - off : 8;

            memcpy(key + off, &r, chunk);
        }
    }

    return ks;
}

/* Sattolo: a single n-cycle, so the GET chain visits every key once. */
static size_t *make_cycle(size_t n, uint64_t seed)
{
    size_t *next = xmalloc(n * sizeof(size_t));
    uint64_t state = seed;

    for (size_t i = 0; i < n; i++)
        next[i] = i;

    for (size_t i = n - 1; i > 0; i--)
    {
        size_t j = (size_t)(splitmix64(&state) % i);
        size_t tmp = next[i];

        next[i] = next[j];
        next[j] = tmp;
    }

    return next;
}

/* Value i starts with key next[i] (drives the chain), padded with a byte
   derived from i so verify() catches values landing on the wrong key. */
static rows_t make_values(const rows_t *ks, const size_t *next, size_t len)
{
    rows_t vs = {xmalloc(ks->count * len), ks->count, len};

    for (size_t i = 0; i < ks->count; i++)
    {
        uint8_t *v = vs.data + i * len;

        memcpy(v, row_at(ks, next[i]), ks->len);
        memset(v + ks->len, (int)(i & 0xFF), len - ks->len);
    }

    return vs;
}

/* ------------------------------------------------------------------- ops */

static double *make_zipf_cdf(size_t n, double s)
{
    double *cdf = xmalloc(n * sizeof(double));
    double sum = 0.0;

    for (size_t i = 0; i < n; i++)
    {
        sum += 1.0 / pow((double)(i + 1), s);
        cdf[i] = sum;
    }

    for (size_t i = 0; i < n; i++)
        cdf[i] /= sum;

    return cdf;
}

static size_t zipf_draw(const double *cdf, size_t n, uint64_t *state)
{
    double u = (double)(splitmix64(state) >> 11) * 0x1p-53;
    size_t lo = 0, hi = n - 1;

    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;

        if (cdf[mid] < u)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo;
}

/* Sampling happens here, never in a timed loop. Rank r maps to key r; keys
   are random bytes, so hot keys land on a handful of random shards — the
   mutex contention that follows is the point of the zipf runs. */
static uint32_t *make_ops(size_t n_ops, size_t items, unsigned set_pct,
                          const double *zipf_cdf, uint64_t seed)
{
    uint32_t *ops = xmalloc(n_ops * sizeof(uint32_t));
    uint64_t state = seed;

    for (size_t i = 0; i < n_ops; i++)
    {
        size_t idx = zipf_cdf ? zipf_draw(zipf_cdf, items, &state)
                              : (size_t)(splitmix64(&state) % items);
        bool set = (splitmix64(&state) % 100) < set_pct;

        ops[i] = (uint32_t)idx | (set ? OP_SET : 0);
    }

    return ops;
}

/* ------------------------------------------------------------ kv helpers */

static inline void set_row(kv_t *kv, const rows_t *ks, const rows_t *vs,
                           size_t ki, size_t vi)
{
    kv_set(kv, row_at(ks, ki), ks->len, row_at(vs, vi), vs->len,
           TAG_FLAGS, TAG_EXP);
}

static kv_t *build_kv(const rows_t *ks, const rows_t *vs)
{
    kv_t *kv = kv_init();

    if (!kv)
    {
        fprintf(stderr, "kv_init failed\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < ks->count; i++)
        set_row(kv, ks, vs, i, i);

    return kv;
}

/* Every GET below reads the first 8 value bytes: a server copies the value
   out, so the value's cache line is part of the request cost. */
static inline uint64_t read_word(const uint8_t *v)
{
    uint64_t w;

    memcpy(&w, v, sizeof w);
    return w;
}

/* dst capacity for every GET. Values here never exceed value_len, so this
   mirrors option 1: a buffer sized to the max value, never too small. */
static size_t value_cap;

static inline kv_error_t get_into(kv_t *kv, const uint8_t *key, size_t key_len,
                                  uint8_t *buf, size_t *vlen)
{
    uint16_t flags;
    uint32_t exp;

    return kv_get(kv, key, key_len, buf, value_cap, vlen, &flags, &exp);
}

/* Runs ops[begin, end). GETs copy into buf under the shard lock, so this is
   safe with concurrent writers. */
static uint64_t run_ops(kv_t *kv, const rows_t *ks, const rows_t *vs,
                        const uint32_t *ops, size_t begin, size_t end, uint8_t *buf)
{
    uint64_t acc = 0;

    for (size_t i = begin; i < end; i++)
    {
        uint32_t op = ops[i];
        size_t idx = op & OP_IDX;

        if (op & OP_SET)
        {
            set_row(kv, ks, vs, idx, idx);
        }
        else
        {
            size_t vlen;

            if (get_into(kv, row_at(ks, idx), ks->len, buf, &vlen) == KV_OK)
                acc += read_word(buf);
        }
    }

    return acc;
}

/* ---------------------------------------------------------- correctness */

/* Numbers from a kv that loses or corrupts entries are meaningless, so this
   runs first and aborts the bench on any failure. */
static bool verify(kv_t *kv, const rows_t *ks, const rows_t *vs, const rows_t *absent)
{
    size_t missing = 0, bad_len = 0, bad_bytes = 0, bad_meta = 0, false_hits = 0;
    size_t small_ok = 0;
    uint8_t *buf = xmalloc(value_cap);

    for (size_t i = 0; i < ks->count; i++)
    {
        size_t vlen;
        uint16_t flags;
        uint32_t exp;

        if (kv_get(kv, row_at(ks, i), ks->len, buf, value_cap, &vlen, &flags, &exp) != KV_OK)
        {
            missing++;
            continue;
        }

        if (vlen != vs->len)
        {
            bad_len++;
            continue;
        }

        bad_bytes += memcmp(buf, row_at(vs, i), vlen) != 0;
        bad_meta += flags != TAG_FLAGS || exp != TAG_EXP;
    }

    /* dst one byte short must be refused, never truncated */
    for (size_t i = 0; i < 1000 && i < ks->count; i++)
    {
        size_t vlen;
        uint16_t flags;
        uint32_t exp;

        small_ok += kv_get(kv, row_at(ks, i), ks->len, buf, vs->len - 1,
                           &vlen, &flags, &exp) == KV_OK;
    }

    for (size_t i = 0; i < absent->count; i++)
    {
        size_t vlen;

        false_hits += get_into(kv, row_at(absent, i), absent->len, buf, &vlen) == KV_OK;
    }

    free(buf);

    if (missing | bad_len | bad_bytes | bad_meta | false_hits | small_ok)
    {
        fprintf(stderr,
                "verify FAILED: missing=%zu bad_len=%zu bad_bytes=%zu "
                "bad_flags/exp=%zu false_hits=%zu short_dst_accepted=%zu (of %zu)\n",
                missing, bad_len, bad_bytes, bad_meta, false_hits, small_ok, ks->count);
        return false;
    }

    printf("verify: ok (%zu keys round-tripped, %zu absent keys missed)\n",
           ks->count, absent->count);
    return true;
}

/* ------------------------------------------------------ single-threaded */

/* Fresh kv per rep: every shard grows from its initial size, so resize cost
   is in here. kv_init/kv_destroy are outside the timer. */
static double bench_set_insert(const bench_config_t *cfg, const rows_t *ks,
                               const rows_t *vs)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        kv_t *kv = kv_init();

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < ks->count; i++)
            set_row(kv, ks, vs, i, i);

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        keep_min(&best, rep, (double)(end - start) / (double)ks->count);
        kv_destroy(kv);
    }

    return best;
}

/* Same size as the stored value: the in-place path, no allocator traffic. */
static double bench_set_overwrite(const bench_config_t *cfg, kv_t *kv,
                                  const rows_t *ks, const rows_t *vs)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < ks->count; i++)
            set_row(kv, ks, vs, i, i);

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        keep_min(&best, rep, (double)(end - start) / (double)ks->count);
    }

    return best;
}

/* Larger value than the item holds: free + alloc in another slab class.
   Fresh kv per rep, otherwise rep 2 would hit the in-place path. */
static double bench_set_grow(const bench_config_t *cfg, const rows_t *ks,
                             const rows_t *vs)
{
    size_t big_len = vs->len * GROW_FACTOR;
    uint8_t *big = xmalloc(big_len);
    double best = 0.0;

    memset(big, 0x5A, big_len);

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        kv_t *kv = build_kv(ks, vs);

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < ks->count; i++)
            kv_set(kv, row_at(ks, i), ks->len, big, big_len, TAG_FLAGS, TAG_EXP);

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        keep_min(&best, rep, (double)(end - start) / (double)ks->count);
        kv_destroy(kv);
    }

    free(big);
    return best;
}

/* Independent lookups: throughput, not latency. Keys walked sequentially;
   shard and bucket order are already random via the hash. */
static double bench_get_hit(const bench_config_t *cfg, kv_t *kv, const rows_t *ks)
{
    uint8_t *buf = xmalloc(value_cap);
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        uint64_t checksum = 0;
        size_t misses = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < ks->count; i++)
        {
            size_t vlen;

            if (get_into(kv, row_at(ks, i), ks->len, buf, &vlen) == KV_OK)
                checksum += read_word(buf);
            else
                misses++;
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(checksum);

        if (misses)
            fprintf(stderr, "ERROR: %zu GET hit misses\n", misses);

        keep_min(&best, rep, (double)(end - start) / (double)ks->count);
    }

    free(buf);
    return best;
}

static double bench_get_miss(const bench_config_t *cfg, kv_t *kv, const rows_t *absent)
{
    uint8_t *buf = xmalloc(value_cap);
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        uint64_t hits = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < absent->count; i++)
        {
            size_t vlen;

            hits += get_into(kv, row_at(absent, i), absent->len, buf, &vlen) == KV_OK;
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(hits);

        if (hits)
            fprintf(stderr, "ERROR: %" PRIu64 " false positives\n", hits);

        keep_min(&best, rep, (double)(end - start) / (double)absent->count);
    }

    free(buf);
    return best;
}

/* Next key comes out of the previous value: nothing overlaps, so this is
   true per-GET latency including the lock round trip. */
static double bench_get_chain(const bench_config_t *cfg, kv_t *kv, const rows_t *ks)
{
    /* Two buffers, alternated: the next key is read out of the previous dst,
       and dst must not alias the key being looked up. */
    uint8_t *buf[2] = {xmalloc(value_cap), xmalloc(value_cap)};
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        const uint8_t *key = row_at(ks, 0);
        size_t steps = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < ks->count; i++)
        {
            uint8_t *dst = buf[i & 1];
            size_t vlen;

            if (get_into(kv, key, ks->len, dst, &vlen) != KV_OK)
                break;

            key = dst;
            steps++;
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(read_word(key));

        if (steps != ks->count)
        {
            fprintf(stderr, "ERROR: chain broke after %zu of %zu steps\n", steps, ks->count);
            if (steps == 0)
                break;
        }

        keep_min(&best, rep, (double)(end - start) / (double)steps);
    }

    free(buf[0]);
    free(buf[1]);
    return best;
}

static double bench_delete(const bench_config_t *cfg, const rows_t *ks, const rows_t *vs)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        kv_t *kv = build_kv(ks, vs);
        size_t misses = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t i = 0; i < ks->count; i++)
            misses += kv_delete(kv, row_at(ks, i), ks->len) != KV_OK;

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        if (misses)
            fprintf(stderr, "ERROR: %zu deletes found nothing\n", misses);

        keep_min(&best, rep, (double)(end - start) / (double)ks->count);
        kv_destroy(kv);
    }

    return best;
}

/* Steady state at full size: evict the oldest key, insert a new one.
   Exercises table delete + insert and slab free + reuse together. FIFO
   eviction keeps the harness free of a random slot-array miss; the free
   order it gives the allocator is the slab bench's FIFO case.
   Reported per op: each iteration is 2 ops. */
static double bench_churn(const bench_config_t *cfg, const rows_t *ks,
                          const rows_t *vs, const rows_t *fresh)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        kv_t *kv = build_kv(ks, vs);
        size_t lost = 0;

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        for (size_t k = 0; k < fresh->count; k++)
        {
            lost += kv_delete(kv, row_at(ks, k), ks->len) != KV_OK;
            kv_set(kv, row_at(fresh, k), fresh->len, row_at(vs, k), vs->len,
                   TAG_FLAGS, TAG_EXP);
        }

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        if (lost)
            fprintf(stderr, "ERROR: churn evicted %zu keys that were not there\n", lost);

        keep_min(&best, rep, (double)(end - start) / (double)(2 * fresh->count));
        kv_destroy(kv);
    }

    return best;
}

static double bench_mixed(const bench_config_t *cfg, kv_t *kv, const rows_t *ks,
                          const rows_t *vs, const uint32_t *ops)
{
    uint8_t *buf = xmalloc(value_cap);
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        COMPILER_BARRIER();
        uint64_t start = now_ns();

        sink_u64(run_ops(kv, ks, vs, ops, 0, STREAM_OPS, buf));

        uint64_t end = now_ns();
        COMPILER_BARRIER();

        keep_min(&best, rep, (double)(end - start) / (double)STREAM_OPS);
    }

    free(buf);
    return best;
}

/* -------------------------------------------------------------- latency */

static double clock_ns; /* clock_gettime cost, subtracted from each sample */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static double pct(const uint64_t *sorted, size_t n, double p)
{
    size_t i = (size_t)((double)n * p);
    double x = (double)sorted[i < n ? i : n - 1] - clock_ns;

    return x < 0 ? 0 : x;
}

static void report_latency(const char *label, uint64_t *lat, size_t n)
{
    qsort(lat, n, sizeof(uint64_t), cmp_u64);
    printf("%-14s p50 %6.0f  p99 %6.0f  p99.9 %7.0f  p99.99 %8.0f  max %9.0f ns\n",
           label, pct(lat, n, 0.50), pct(lat, n, 0.99), pct(lat, n, 0.999),
           pct(lat, n, 0.9999), pct(lat, n, 1.0));
}

/* Per-op timing. Absolute p50 is approximate (the timer overlaps with the
   op); the tail is the signal. SET insert tail = shard resize on the
   request path. */
static void bench_latency(const rows_t *ks, const rows_t *vs)
{
    size_t n = ks->count;
    uint64_t *lat = xmalloc(n * sizeof(uint64_t));
    uint64_t acc = 0;
    kv_t *kv = kv_init();

    for (size_t i = 0; i < n; i++)
    {
        uint64_t t0 = now_ns();
        set_row(kv, ks, vs, i, i);
        lat[i] = now_ns() - t0;
    }
    report_latency("SET insert", lat, n);

    uint8_t *buf = xmalloc(value_cap);

    for (size_t i = 0; i < n; i++)
    {
        size_t vlen;

        uint64_t t0 = now_ns();
        if (get_into(kv, row_at(ks, i), ks->len, buf, &vlen) == KV_OK)
            acc += read_word(buf);
        lat[i] = now_ns() - t0;
    }
    report_latency("GET hit", lat, n);
    free(buf);

    for (size_t i = 0; i < n; i++)
    {
        uint64_t t0 = now_ns();
        set_row(kv, ks, vs, i, i);
        lat[i] = now_ns() - t0;
    }
    report_latency("SET overwrite", lat, n);

    sink_u64(acc);
    kv_destroy(kv);
    free(lat);
}

/* -------------------------------------------------------- multi-threaded */

typedef struct mt_arg
{
    kv_t *kv;
    const rows_t *ks;
    const rows_t *vs;
    const uint32_t *ops;
    size_t offset;
    int cpu;
    pthread_barrier_t *barrier;
    uint64_t start;
    uint64_t end;
    uint64_t hits;
} mt_arg_t;

/* Every thread runs the whole stream, starting at its own offset, so threads
   share one read-only 16 MB stream instead of one each. */
static void *mt_worker(void *p)
{
    mt_arg_t *a = p;
    uint8_t *buf = xmalloc(value_cap); /* thread-private: its own cache lines */

    pin_to_cpu(a->cpu);
    pthread_barrier_wait(a->barrier);

    a->start = now_ns();
    a->hits = run_ops(a->kv, a->ks, a->vs, a->ops, a->offset, STREAM_OPS, buf) +
              run_ops(a->kv, a->ks, a->vs, a->ops, 0, a->offset, buf);
    a->end = now_ns();

    free(buf);
    return NULL;
}

/* Aggregate throughput = all ops / (last finish - first start). */
static void bench_mt(const bench_config_t *cfg, kv_t *kv, const rows_t *ks,
                     const rows_t *vs, const uint32_t *ops, const char *label)
{
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    size_t ncpu = online > 0 ? (size_t)online : 1;
    pthread_t *tid = xmalloc(cfg->max_threads * sizeof(pthread_t));
    mt_arg_t *args = xmalloc(cfg->max_threads * sizeof(mt_arg_t));
    double base = 0.0;
    size_t t = 1;

    printf("%s\n", label);

    for (;;)
    {
        double best = 0.0;

        for (size_t rep = 0; rep < cfg->reps; rep++)
        {
            pthread_barrier_t barrier;

            pthread_barrier_init(&barrier, NULL, (unsigned)t);

            for (size_t i = 0; i < t; i++)
            {
                args[i] = (mt_arg_t){
                    .kv = kv,
                    .ks = ks,
                    .vs = vs,
                    .ops = ops,
                    .offset = i * STREAM_OPS / t,
                    .cpu = (int)(i % ncpu),
                    .barrier = &barrier,
                };

                if (pthread_create(&tid[i], NULL, mt_worker, &args[i]) != 0)
                {
                    fprintf(stderr, "pthread_create failed\n");
                    exit(EXIT_FAILURE);
                }
            }

            uint64_t start = UINT64_MAX, end = 0;

            for (size_t i = 0; i < t; i++)
            {
                pthread_join(tid[i], NULL);
                if (args[i].start < start)
                    start = args[i].start;
                if (args[i].end > end)
                    end = args[i].end;
            }

            pthread_barrier_destroy(&barrier);

            double mops = (double)(t * STREAM_OPS) * 1e3 / (double)(end - start);
            if (mops > best)
                best = mops;
        }

        if (t == 1)
            base = best;

        printf("  threads %3zu  %8.2f M ops/s  %5.2fx\n", t, best, best / base);

        if (t >= cfg->max_threads)
            break;
        t = (t * 2 > cfg->max_threads) ? cfg->max_threads : t * 2;
    }

    free(args);
    free(tid);
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

int main(int argc, char **argv)
{
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    bench_config_t cfg = {
        DEFAULT_ITEMS,
        DEFAULT_KEY_LEN,
        DEFAULT_VALUE_LEN,
        DEFAULT_REPS,
        online > 0 ? (size_t)online : 1,
    };

    if (argc > 6)
    {
        fprintf(stderr, "usage: %s [items] [key_len] [value_len] [reps] [max_threads]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2)
        cfg.items = parse_size(argv[1], "items");
    if (argc >= 3)
        cfg.key_len = parse_size(argv[2], "key_len");
    if (argc >= 4)
        cfg.value_len = parse_size(argv[3], "value_len");
    if (argc >= 5)
        cfg.reps = parse_size(argv[4], "reps");
    if (argc >= 6)
        cfg.max_threads = parse_size(argv[5], "max_threads");

    if (cfg.items > OP_IDX)
    {
        fprintf(stderr, "items must be <= %u\n", OP_IDX);
        return EXIT_FAILURE;
    }

    /* value carries the next chain key and read_word() reads 8 bytes */
    if (cfg.value_len < cfg.key_len || cfg.value_len < 8)
    {
        fprintf(stderr, "value_len must be >= key_len and >= 8\n");
        return EXIT_FAILURE;
    }

    value_cap = cfg.value_len;

    pin_to_cpu(0);

    uint64_t t0 = now_ns();
    for (int i = 0; i < 100000; i++)
        (void)now_ns();
    clock_ns = (double)(now_ns() - t0) / 100000.0;

    rows_t keys = make_keys(cfg.items, cfg.key_len, 0x5EED0001);
    rows_t absent = make_keys(cfg.items, cfg.key_len, 0x5EED0002);
    rows_t fresh = make_keys(cfg.items, cfg.key_len, 0x5EED0005);
    size_t *next = make_cycle(cfg.items, 0x5EED0003);
    rows_t vals = make_values(&keys, next, cfg.value_len);
    free(next);

    printf("items=%zu key_len=%zu value_len=%zu reps=%zu max_threads=%zu\n"
           "payload=%.1f MiB, clock_gettime=%.1f ns\n\n",
           cfg.items, cfg.key_len, cfg.value_len, cfg.reps, cfg.max_threads,
           (double)(cfg.items * (cfg.key_len + cfg.value_len)) / 1048576.0, clock_ns);

    /* First, before any other kv exists: the delta is the kv alone
       (slab pages + tables, including old tables freed during growth that
       the process still holds). */
    long rss0 = rss_kb();
    kv_t *kv = build_kv(&keys, &vals);
    long rss1 = rss_kb();

    if (rss0 >= 0 && rss1 >= 0)
    {
        double bytes = (double)(rss1 - rss0) * 1024.0;
        double per_item = bytes / (double)cfg.items;
        size_t payload = cfg.key_len + cfg.value_len;

        printf("footprint: %.1f MiB resident, %.1f B/item for %zu B payload (%.2fx)\n",
               bytes / 1048576.0, per_item, payload, per_item / (double)payload);
    }

    if (!verify(kv, &keys, &vals, &absent))
        return EXIT_FAILURE;

    printf("\n== single thread ==\n");
    report("SET insert", bench_set_insert(&cfg, &keys, &vals));
    report("SET overwrite", bench_set_overwrite(&cfg, kv, &keys, &vals));
    report("SET grow", bench_set_grow(&cfg, &keys, &vals));
    report("GET hit", bench_get_hit(&cfg, kv, &keys));
    report("GET miss", bench_get_miss(&cfg, kv, &absent));
    report("GET chain", bench_get_chain(&cfg, kv, &keys));
    report("DELETE", bench_delete(&cfg, &keys, &vals));
    report("CHURN", bench_churn(&cfg, &keys, &vals, &fresh));

    double *cdf = make_zipf_cdf(cfg.items, ZIPF_S);

    struct
    {
        const char *name;
        unsigned set_pct;
        const double *cdf;
        uint32_t *ops;
    } mix[] = {
        {"90/10 uniform", 10, NULL, NULL},
        {"90/10 zipf", 10, cdf, NULL},
        {"50/50 uniform", 50, NULL, NULL},
        {"50/50 zipf", 50, cdf, NULL},
    };
    size_t n_mix = sizeof(mix) / sizeof(mix[0]);

    for (size_t i = 0; i < n_mix; i++)
        mix[i].ops = make_ops(STREAM_OPS, cfg.items, mix[i].set_pct, mix[i].cdf,
                              0x5EED0010 + i);

    printf("\n== mixed GET/SET, steady state (%lu ops, zipf s=%.2f) ==\n",
           STREAM_OPS, ZIPF_S);
    for (size_t i = 0; i < n_mix; i++)
        report(mix[i].name, bench_mixed(&cfg, kv, &keys, &vals, mix[i].ops));

    printf("\n== per-op latency (clock overhead subtracted) ==\n");
    bench_latency(&keys, &vals);

    printf("\n== thread scaling, shared kv ==\n");
    for (size_t i = 0; i < n_mix; i++)
        bench_mt(&cfg, kv, &keys, &vals, mix[i].ops, mix[i].name);

    for (size_t i = 0; i < n_mix; i++)
        free(mix[i].ops);
    free(cdf);

    kv_destroy(kv);
    free(vals.data);
    free(fresh.data);
    free(absent.data);
    free(keys.data);

    return EXIT_SUCCESS;
}