/*
 * bench_slab.c - benchmark + sanity harness for slab_alloc.
 *
 *   cc -O2 -std=c11 -o bench_slab bench_slab.c slab_alloc.c
 *   ./bench_slab
 *
 * Backend selection is a loop-invariant int, not a function pointer: the
 * branch is perfectly predicted and costs less than an indirect call would.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "item.h"
#include "slab_alloc.h"

/* ------------------------------------------------------------------ knobs */

#define LIVE_OBJECTS 100000 /* steady-state working set                     */
#define CHURN_OPS 2000000   /* alloc+free pairs in the churn benchmark      */
#define LAT_SAMPLES 200000  /* individually timed ops for the latency test  */
#define FIXED_SIZE 64
#define REPEATS 3

/* ---------------------------------------------------------------- backend */

enum
{
    BE_SLAB = 0,
    BE_LIBC = 1
};

static const char *be_name[] = {"slab", "libc"};

static slab_allocator_t *A;
static double clock_ns; /* measured clock_gettime overhead, subtracted from latencies */

/*
 * Both backends allocate the same logical object: an item header plus
 * `data_len` payload bytes. libc therefore pays ITEM_OVERHEAD too, so the
 * comparison stays like-for-like.
 */
static inline item_t *be_alloc(int be, size_t data_len)
{
    if (be == BE_SLAB)
        return slab_allocator_alloc_item(A, data_len);

    item_t *it = malloc(ITEM_SIZE + data_len);
    if (it)
        it->slab_ptr = NULL;
    return it;
}

static inline void be_free(int be, item_t *it)
{
    if (be == BE_SLAB)
        slab_allocator_free_item(it);
    else
        free(it);
}

/*
 * slab_ptr sits at offset 0 so that any caller reading the item header pulls
 * it into L1 as a side effect. If it ever moves past offset 15 the header can
 * straddle a cache line and this stops being true for a share of items.
 */
_Static_assert(offsetof(item_t, slab_ptr) == 0,
               "slab_ptr must be first or free() takes a cold miss");

/*
 * Models the hashtable lookup that precedes any real free: the caller has
 * already compared the key, so the item's first cache line is hot and
 * it->slab costs nothing to read.
 *
 * Without this the benchmark frees pointers it never looked at, the line is
 * cold, and an in-item back-pointer measures no better than a hidden header.
 * That would be an artifact of the harness, not a property of the design.
 * Set TOUCH=0 in the environment to see the difference.
 */
static int touch_before_free = 1;

static inline uint64_t item_lookup(item_t *it)
{
    return (uint64_t)it->key_len ^ it->value_len ^ it->expire_at;
}

static uint64_t sink;

static inline void be_free_after_lookup(int be, item_t *it)
{
    if (touch_before_free)
        sink += item_lookup(it);
    be_free(be, it);
}

/* ----------------------------------------------------------------- timing */

static inline double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return -1;
    long total, res;
    if (fscanf(f, "%ld %ld", &total, &res) != 2)
        res = -1;
    fclose(f);
    return res < 0 ? -1 : res * (long)(sysconf(_SC_PAGESIZE) / 1024);
}

/* -------------------------------------------------------------------- rng */

static uint64_t rng_s;

static inline void rng_seed(uint64_t s)
{
    rng_s = s ? s : 0x243F6A8885A308D3ULL;
}

static inline uint64_t rng_next(void)
{
    uint64_t x = rng_s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_s = x;
}

/*
 * Size mix modelled on a cache workload: mostly small values, a long tail.
 * Precomputed so every backend replays the identical sequence.
 */
static size_t sample_size(void)
{
    uint32_t r = (uint32_t)(rng_next() >> 32) % 100;
    if (r < 70)
        return 48 + rng_next() % 80; /*  48..127   */
    if (r < 90)
        return 128 + rng_next() % 896; /* 128..1023  */
    if (r < 99)
        return 1024 + rng_next() % 7168; /*  1K..8K    */
    return 8192 + rng_next() % 57344;    /*  8K..64K   */
}

static size_t *sizes; /* CHURN_OPS + LIVE_OBJECTS entries */
static size_t sizes_n;

static void sizes_build_n(int mixed, size_t live, size_t ops)
{
    free(sizes);
    sizes_n = ops + live;
    sizes = malloc(sizeof(size_t) * sizes_n);
    if (!sizes)
        abort();

    rng_seed(0xC0FFEE);
    for (size_t i = 0; i < sizes_n; i++)
        sizes[i] = mixed ? sample_size() : FIXED_SIZE;
}

static void sizes_build(int mixed)
{
    sizes_build_n(mixed, LIVE_OBJECTS, CHURN_OPS);
}

/* ------------------------------------------------------------ correctness */

/*
 * Writes a slot-derived pattern into every live chunk and verifies it later.
 * Catches overlapping chunks, double handout, and off-by-one in chunk_size.
 * Also checks payload alignment, which the current header layout gets wrong
 * as soon as alignof(item_t) exceeds 8.
 */
static void check_integrity(int be, size_t align)
{
    enum
    {
        N = 20000
    };
    static item_t *p[N];
    static size_t sz[N];

    rng_seed(0xBEEF);
    for (size_t i = 0; i < N; i++)
    {
        sz[i] = sample_size();
        p[i] = be_alloc(be, sz[i]);
        if (!p[i])
        {
            fprintf(stderr, "alloc failed: %zu data bytes\n", sz[i]);
            abort();
        }
        if ((uintptr_t)p[i] % align)
        {
            fprintf(stderr, "misaligned item: %p not %zu-aligned\n",
                    (void *)p[i], align);
            abort();
        }
        /* the allocator owns it->slab; the caller owns the rest */
        if (be == BE_SLAB && p[i]->slab_ptr == NULL)
        {
            fprintf(stderr, "slot %zu: allocator left slab_ptr NULL\n", i);
            abort();
        }
        p[i]->key_len = (uint16_t)(i & 0xFF);
        p[i]->flags = (uint16_t)(i & 0x7);
        p[i]->value_len = (uint32_t)sz[i];
        p[i]->expire_at = (uint32_t)(i * 7u);
        memset(p[i]->data, (int)(i & 0xFF), sz[i]);
    }

    for (size_t i = 0; i < N; i++)
    {
        item_t *it = p[i];

        /* header intact: catches a later allocation overlapping this chunk */
        if (it->key_len != (uint16_t)(i & 0xFF) ||
            it->value_len != (uint32_t)sz[i] ||
            it->expire_at != (uint32_t)(i * 7u))
        {
            fprintf(stderr, "slot %zu: item header clobbered\n", i);
            abort();
        }
        if (be == BE_SLAB && it->slab_ptr == NULL)
        {
            fprintf(stderr, "slot %zu: slab_ptr clobbered\n", i);
            abort();
        }

        uint8_t want = (uint8_t)(i & 0xFF);
        for (size_t j = 0; j < sz[i]; j++)
        {
            if (it->data[j] != want)
            {
                fprintf(stderr, "slot %zu payload byte %zu = %u, want %u\n",
                        i, j, it->data[j], want);
                abort();
            }
        }
    }

    for (size_t i = 0; i < N; i++)
        be_free_after_lookup(be, p[i]);

    printf("  integrity(%s): ok  (%d items, %zu-byte alignment, headers intact)\n",
           be_name[be], N, align);
}

/* ------------------------------------------------------------- benchmarks */

/* alloc n, then free in reverse order - best case for a LIFO freelist */
static double bench_lifo(int be, item_t **slots, size_t n)
{
    double t0 = now_ns();
    for (size_t i = 0; i < n; i++)
        slots[i] = be_alloc(be, sizes[i]);
    for (size_t i = n; i-- > 0;)
        be_free(be, slots[i]);
    return (now_ns() - t0) / (double)(2 * n);
}

/* alloc n, then free in allocation order - walks the freelist backwards */
static double bench_fifo(int be, item_t **slots, size_t n)
{
    double t0 = now_ns();
    for (size_t i = 0; i < n; i++)
        slots[i] = be_alloc(be, sizes[i]);
    for (size_t i = 0; i < n; i++)
        be_free(be, slots[i]);
    return (now_ns() - t0) / (double)(2 * n);
}

/* alloc + touch both ends + free - exposes first-touch page faults */
static double bench_touch(int be, item_t **slots, size_t n)
{
    double t0 = now_ns();
    for (size_t i = 0; i < n; i++)
    {
        item_t *it = be_alloc(be, sizes[i]);
        it->data[0] = (uint8_t)i;
        it->data[sizes[i] - 1] = (uint8_t)i;
        slots[i] = it;
    }
    for (size_t i = n; i-- > 0;)
        be_free_after_lookup(be, slots[i]);
    return (now_ns() - t0) / (double)(2 * n);
}

/*
 * Steady state: hold LIVE_OBJECTS live, then repeatedly evict a random slot
 * and allocate a replacement. This is the shape a KV store actually sees.
 */
static double bench_churn(int be, item_t **slots, size_t n, size_t ops)
{
    for (size_t i = 0; i < n; i++)
        slots[i] = be_alloc(be, sizes[i]);

    rng_seed(0x5EED);
    double t0 = now_ns();
    for (size_t k = 0; k < ops; k++)
    {
        size_t slot = rng_next() % n;
        be_free_after_lookup(be, slots[slot]);
        slots[slot] = be_alloc(be, sizes[n + k]);
    }
    double ns = (now_ns() - t0) / (double)(2 * ops);

    for (size_t i = 0; i < n; i++)
        be_free(be, slots[i]);
    return ns;
}

/* ---------------------------------------------------------------- latency */

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/*
 * Per-op timing. Absolute values include ~2x clock_gettime overhead, printed
 * below so it can be subtracted; the tail is what matters here - a spike at
 * p99.9 means a slab was created on the critical path.
 */
static void bench_latency(int be, item_t **slots, size_t n)
{
    static double lat[LAT_SAMPLES];

    for (size_t i = 0; i < n; i++)
        slots[i] = be_alloc(be, sizes[i]);

    rng_seed(0x1A7E);
    for (size_t k = 0; k < LAT_SAMPLES; k++)
    {
        size_t slot = rng_next() % n;
        be_free_after_lookup(be, slots[slot]);

        double t0 = now_ns();
        slots[slot] = be_alloc(be, sizes[n + k]);
        lat[k] = now_ns() - t0;
    }

    qsort(lat, LAT_SAMPLES, sizeof(double), cmp_dbl);
#define ADJ(x) ((x) - clock_ns < 0 ? 0 : (x) - clock_ns)
    printf("  %-5s alloc latency ns   p50 %6.0f  p99 %6.0f  p99.9 %7.0f  max %8.0f\n",
           be_name[be],
           ADJ(lat[LAT_SAMPLES / 2]),
           ADJ(lat[(size_t)(LAT_SAMPLES * 0.99)]),
           ADJ(lat[(size_t)(LAT_SAMPLES * 0.999)]),
           ADJ(lat[LAT_SAMPLES - 1]));
#undef ADJ

    for (size_t i = 0; i < n; i++)
        be_free(be, slots[i]);
}

/* -------------------------------------------------------------- footprint */

/*
 * Internal fragmentation is deterministic: compare what the caller asked for
 * against the class chunk actually handed out. No timing noise involved.
 */
static void report_fragmentation(size_t n)
{
    size_t req = 0, usable = 0, worst_req = 0, worst_use = 0;
    double worst = 0;

    for (size_t i = 0; i < n; i++)
    {
        size_t u = slab_allocator_usable_size(A, sizes[i]);
        req += sizes[i];
        usable += u;
        double w = (double)u / (double)sizes[i];
        if (w > worst)
        {
            worst = w;
            worst_req = sizes[i];
            worst_use = u;
        }
    }

    printf("  slab  internal frag     %.1f%% wasted  (worst single request: %zu -> %zu, %.2fx)\n",
           100.0 * (double)(usable - req) / (double)usable,
           worst_req, worst_use, worst);
}

/*
 * RSS must be measured in a fresh process: once pages are resident from an
 * earlier phase, a delta in the same process is meaningless.
 */
static void footprint_child(int be, size_t n)
{
    item_t **slots = malloc(sizeof(item_t *) * n);
    if (!slots)
        _exit(1);

    if (be == BE_SLAB)
        A = slab_allocator_init();

    long before = rss_kb();
    size_t requested = 0;

    for (size_t i = 0; i < n; i++)
    {
        slots[i] = be_alloc(be, sizes[i]);
        memset(slots[i]->data, 1, sizes[i]);
        requested += ITEM_SIZE + sizes[i];
    }

    long after = rss_kb();
    double req_mb = (double)requested / 1048576.0;
    double rss_mb = (double)(after - before) / 1024.0;

    printf("  %-5s rss                requested %7.1f MB   resident %7.1f MB   overhead %+6.1f%%\n",
           be_name[be], req_mb, rss_mb, 100.0 * (rss_mb - req_mb) / req_mb);
    fflush(stdout);
    _exit(0);
}

/*
 * fork() is not enough: inherited COW pages already count toward the child's
 * RSS, so writes to them show no growth. Re-exec for a genuinely cold process.
 */
static void bench_footprint(int be, int mixed, size_t n)
{
    char a1[32], a2[32], a3[32];
    snprintf(a1, sizeof a1, "%d", be);
    snprintf(a2, sizeof a2, "%d", mixed);
    snprintf(a3, sizeof a3, "%zu", n);
    char *argv[] = {"bench_slab", "--footprint", a1, a2, a3, NULL};

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
    {
        execv("/proc/self/exe", argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}

/* ------------------------------------------------------------- cold start */

/*
 * How much memory does the allocator make resident before it has served any
 * real load? Run cold, in its own process. A slab that threads its freelist
 * at construction dirties its whole 2 MB region up front, so the answer is
 * "one slab per touched class" rather than "one page".
 */
static void coldstart_child(int classes)
{
    long base = rss_kb();
    A = slab_allocator_init();
    long after_init = rss_kb();

    if (classes)
        for (size_t sz = 48; sz <= 65536; sz = sz + sz / 4 + 1)
            (void)slab_allocator_alloc_item(A, sz);
    else
        (void)slab_allocator_alloc_item(A, 64);

    long after = rss_kb();
    printf("  %-26s init %+5ld KB   first alloc %+8ld KB   total %8ld KB\n",
           classes ? "one object per class" : "one 64 B object",
           after_init - base, after - after_init, after - base);
    fflush(stdout);
    _exit(0);
}

static void bench_coldstart(int classes)
{
    char a1[16];
    snprintf(a1, sizeof a1, "%d", classes);
    char *argv[] = {"bench_slab", "--coldstart", a1, NULL};

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
    {
        execv("/proc/self/exe", argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}

/* --------------------------------------------------------------- profiling */

/*
 * Run exactly one pattern in a loop for `secs`, nothing else. A perf profile
 * of the full suite is a blend of eight phases plus two allocators and tells
 * you very little; this gives a single hot loop to attribute samples to.
 *
 *   ./slab_bench --run churn slab fixed 10
 */
static int run_single(const char *pattern, const char *backend,
                      const char *mix, double secs)
{
    int be = strcmp(backend, "libc") == 0 ? BE_LIBC : BE_SLAB;
    sizes_build(strcmp(mix, "fixed") != 0);

    size_t n = LIVE_OBJECTS;
    item_t **slots = malloc(sizeof(item_t *) * n);
    if (!slots)
        return 1;

    if (be == BE_SLAB)
        A = slab_allocator_init();

    double (*fn)(int, item_t **, size_t) = NULL;
    if (strcmp(pattern, "lifo") == 0)
        fn = bench_lifo;
    else if (strcmp(pattern, "fifo") == 0)
        fn = bench_fifo;
    else if (strcmp(pattern, "touch") == 0)
        fn = bench_touch;
    else if (strcmp(pattern, "churn") == 0)
        fn = NULL;
    else
    {
        fprintf(stderr, "unknown pattern: %s\n", pattern);
        return 2;
    }

    double deadline = now_ns() + secs * 1e9;
    double acc = 0;
    size_t rounds = 0;

    while (now_ns() < deadline)
    {
        acc += fn ? fn(be, slots, n) : bench_churn(be, slots, n, CHURN_OPS / 8);
        rounds++;
    }

    fprintf(stderr, "%s/%s/%s: %zu rounds, %.2f ns/op\n",
            pattern, backend, mix, rounds, acc / (double)rounds);
    return 0;
}

/* ------------------------------------------------------------- sweep */

/*
 * Counter-free locality test, for machines with no PMU (most cloud VMs).
 *
 * If the slab allocator loses to glibc because of cache misses rather than
 * instruction count, the gap must depend on how much of the live set fits in
 * cache: near-parity when everything is in L2, widening as the live set
 * outgrows L3. A flat ratio across three orders of magnitude falsifies the
 * hypothesis without counting a single event.
 */
static void bench_sweep(int mixed)
{
    static const size_t ns[] = {2000, 8000, 32000, 128000, 512000};
    const size_t ops = 400000;

    printf("\n== working-set sweep (%s, random churn) ==\n",
           mixed ? "mixed" : "fixed 64 B");
    printf("  %10s %10s %12s %12s %8s\n",
           "live objs", "live MB", "slab ns/op", "libc ns/op", "ratio");

    for (size_t i = 0; i < sizeof(ns) / sizeof(ns[0]); i++)
    {
        size_t n = ns[i];
        sizes_build_n(mixed, n, ops);

        size_t live = 0;
        for (size_t k = 0; k < n; k++)
            live += ITEM_SIZE + sizes[k];

        item_t **slots = malloc(sizeof(item_t *) * n);
        if (!slots)
            abort();

        A = slab_allocator_init();
        double s1 = bench_churn(BE_SLAB, slots, n, ops);
        double s2 = bench_churn(BE_SLAB, slots, n, ops);
        double sl = s1 < s2 ? s1 : s2;
        slab_allocator_destroy(A);

        double l1 = bench_churn(BE_LIBC, slots, n, ops);
        double l2 = bench_churn(BE_LIBC, slots, n, ops);
        double lc = l1 < l2 ? l1 : l2;

        printf("  %10zu %10.1f %12.2f %12.2f %7.2fx\n",
               n, (double)live / 1048576.0, sl, lc, lc / sl);
        fflush(stdout);
        free(slots);
    }
}

/* ------------------------------------------------------------------- main */

static double best_of(double (*fn)(int, item_t **, size_t),
                      int be, item_t **slots, size_t n)
{
    double best = 1e18;
    for (int r = 0; r < REPEATS; r++)
    {
        double v = fn(be, slots, n);
        if (v < best)
            best = v;
    }
    return best;
}

static void run_suite(const char *label, int mixed, size_t n)
{
    sizes_build(mixed);

    item_t **slots = malloc(sizeof(item_t *) * n);
    if (!slots)
        abort();

    printf("\n== %s  (%zu live objects) ==\n", label, n);
    printf("  %-22s %10s %10s %8s\n", "pattern", "slab ns/op", "libc ns/op", "ratio");

    struct
    {
        const char *name;
        double (*fn)(int, item_t **, size_t);
    } tests[] = {
        {"alloc+free LIFO", bench_lifo},
        {"alloc+free FIFO", bench_fifo},
        {"alloc+touch+free", bench_touch},
    };

    for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++)
    {
        double s = best_of(tests[t].fn, BE_SLAB, slots, n);
        double l = best_of(tests[t].fn, BE_LIBC, slots, n);
        printf("  %-22s %10.2f %10.2f %7.2fx\n", tests[t].name, s, l, l / s);
    }

    double cs = bench_churn(BE_SLAB, slots, n, CHURN_OPS);
    double cl = bench_churn(BE_LIBC, slots, n, CHURN_OPS);
    printf("  %-22s %10.2f %10.2f %7.2fx\n", "random churn", cs, cl, cl / cs);

    printf("\n");
    bench_latency(BE_SLAB, slots, n);
    bench_latency(BE_LIBC, slots, n);

    printf("\n");
    report_fragmentation(n);
    bench_footprint(BE_SLAB, mixed, n);
    bench_footprint(BE_LIBC, mixed, n);

    free(slots);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--sweep") == 0)
    {
        bench_sweep(0);
        bench_sweep(1);
        return 0;
    }

    if (argc >= 5 && strcmp(argv[1], "--run") == 0)
        return run_single(argv[2], argv[3], argv[4],
                          argc > 5 ? atof(argv[5]) : 10.0);

    if (argc == 3 && strcmp(argv[1], "--coldstart") == 0)
        coldstart_child(atoi(argv[2]));

    if (argc == 5 && strcmp(argv[1], "--footprint") == 0)
    {
        int be = atoi(argv[2]);
        sizes_build(atoi(argv[3]));
        footprint_child(be, strtoul(argv[4], NULL, 10));
    }

    {
        const char *t = getenv("TOUCH");
        if (t)
            touch_before_free = atoi(t);
    }
    printf("lookup-before-free: %s\n", touch_before_free ? "on" : "off");

    A = slab_allocator_init();

    /* clock overhead, so per-op numbers can be read honestly */
    double t0 = now_ns();
    for (int i = 0; i < 100000; i++)
        (void)now_ns();
    clock_ns = (now_ns() - t0) / 100000.0;
    printf("clock_gettime overhead: %.1f ns/call (subtracted from latency figures)\n", clock_ns);

    printf("\n== cold start ==\n");
    bench_coldstart(0);
    bench_coldstart(1);

    sizes_build(1);
    printf("\n== sanity ==\n");
    check_integrity(BE_SLAB, 8);
    check_integrity(BE_LIBC, 8);

    run_suite("fixed 64 B", 0, LIVE_OBJECTS);
    run_suite("mixed KV size distribution", 1, LIVE_OBJECTS);

    free(sizes);
    slab_allocator_destroy(A);
    return 0;
}