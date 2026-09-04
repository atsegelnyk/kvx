#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hashtable.h"

#define DEFAULT_BUCKET_COUNT 262144UL
#define DEFAULT_ITEMS_COUNT 1000000UL

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        abort();

    return (uint64_t)ts.tv_sec * 1000000000ULL +
           (uint64_t)ts.tv_nsec;
}

static size_t parse_size(const char *arg)
{
    char *end = NULL;

    unsigned long long value = strtoull(arg, &end, 10);

    if (*arg == '\0' || *end != '\0' || value == 0)
    {
        fprintf(stderr, "invalid number: %s\n", arg);
        exit(EXIT_FAILURE);
    }

    return (size_t)value;
}

static void bench_set(size_t bucket_count, size_t items_count)
{
    hashtable_t *table = hashtable_init(bucket_count, false);

    if (!table)
    {
        fprintf(stderr, "bucket_count must be a power of two\n");
        exit(EXIT_FAILURE);
    }

    uint64_t *keys = malloc(sizeof(uint64_t) * items_count);
    uint64_t *values = malloc(sizeof(uint64_t) * items_count);

    if (!keys || !values)
        abort();

    for (size_t i = 0; i < items_count; i++)
    {
        keys[i] = (uint64_t)i;
        values[i] = (uint64_t)i;
    }

    uint64_t start = now_ns();

    for (size_t i = 0; i < items_count; i++)
    {
        hashtable_set(
            table,
            (const uint8_t *)&keys[i],
            sizeof(keys[i]),
            &values[i]);
    }

    uint64_t end = now_ns();

    double elapsed_ns = (double)(end - start);
    double ns_per_op = elapsed_ns / (double)items_count;
    double ops_per_sec = 1e9 / ns_per_op;

    printf(
        "SET: %.2f ns/op | %.2f M ops/sec\n",
        ns_per_op,
        ops_per_sec / 1e6);

    hashtable_destroy(table);
    free(keys);
    free(values);
}

static void bench_get(size_t bucket_count, size_t items_count)
{
    hashtable_t *table = hashtable_init(bucket_count, false);

    if (!table)
    {
        fprintf(stderr, "bucket_count must be a power of two\n");
        exit(EXIT_FAILURE);
    }

    uint64_t *keys = malloc(sizeof(uint64_t) * items_count);
    uint64_t *values = malloc(sizeof(uint64_t) * items_count);

    if (!keys || !values)
        abort();

    for (size_t i = 0; i < items_count; i++)
    {
        keys[i] = (uint64_t)i;
        values[i] = (uint64_t)i;

        hashtable_set(
            table,
            (const uint8_t *)&keys[i],
            sizeof(keys[i]),
            &values[i]);
    }

    uint64_t checksum = 0;

    uint64_t start = now_ns();

    for (size_t i = 0; i < items_count; i++)
    {
        uint64_t *value = hashtable_get(
            table,
            (const uint8_t *)&keys[i],
            sizeof(keys[i]));

        if (value)
            checksum += *value;
    }

    uint64_t end = now_ns();

    double elapsed_ns = (double)(end - start);
    double ns_per_op = elapsed_ns / (double)items_count;
    double ops_per_sec = 1e9 / ns_per_op;

    printf(
        "GET: %.2f ns/op | %.2f M ops/sec | checksum=%" PRIu64 "\n",
        ns_per_op,
        ops_per_sec / 1e6,
        checksum);

    hashtable_destroy(table);
    free(keys);
    free(values);
}

int main(int argc, char **argv)
{
    size_t bucket_count = DEFAULT_BUCKET_COUNT;
    size_t items_count = DEFAULT_ITEMS_COUNT;

    if (argc >= 2)
        bucket_count = parse_size(argv[1]);

    if (argc >= 3)
        items_count = parse_size(argv[2]);

    if (argc > 3)
    {
        fprintf(
            stderr,
            "usage: %s [bucket_count] [items_count]\n",
            argv[0]);

        return EXIT_FAILURE;
    }

    printf(
        "buckets=%zu items=%zu slots=%zu load=%.2f%%\n",
        bucket_count,
        items_count,
        bucket_count * 8,
        ((double)items_count / (double)(bucket_count * 8)) * 100.0);

    bench_set(bucket_count, items_count);
    bench_get(bucket_count, items_count);

    return EXIT_SUCCESS;
}