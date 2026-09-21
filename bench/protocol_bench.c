/*
 * protocol_bench.c - correctness + throughput harness for protocol_parse_command.
 *
 *   make bench BENCH=protocol_bench
 *   build/bench/bin/bench/protocol_bench [commands] [reps]
 *
 * Every stream is a pipelined buffer of back-to-back commands: the shape a
 * connection's read buffer has under load. The chunked runs replay the same
 * buffer growing one read at a time, so commands split across reads and the
 * parser's INCOMPLETE path gets exercised at realistic rates.
 *
 * No per-op latency: one parse costs about as much as one clock read.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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

#include "protocol.h"

#define DEFAULT_COMMANDS 1000000UL
#define DEFAULT_REPS 5UL

#define STREAM_BUDGET (256UL << 20) /* max bytes per stream; big values get fewer commands */
#define MAX_KEY_LEN 250             /* memcached limit */
#define SCRATCH_LEN 512             /* buffer for single-command checks */

typedef struct bench_config
{
    size_t commands;
    size_t reps;
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

static inline size_t min_size(size_t a, size_t b)
{
    if (a < b)
        return a;
    return b;
}

static inline void keep_min(double *best, size_t rep, double v)
{
    if (rep == 0 || v < *best)
        *best = v;
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

/* ---------------------------------------------------------------- writer */

typedef struct writer
{
    uint8_t *buf;
    size_t pos;
} writer_t;

static void put_bytes(writer_t *w, const void *src, size_t len)
{
    memcpy(w->buf + w->pos, src, len);
    w->pos += len;
}

static void put_str(writer_t *w, const char *s)
{
    put_bytes(w, s, strlen(s));
}

static void put_char(writer_t *w, char c)
{
    w->buf[w->pos] = (uint8_t)c;
    w->pos++;
}

static void put_u64(writer_t *w, uint64_t v)
{
    char digits[20];
    size_t n = 0;

    do
    {
        digits[n] = (char)('0' + v % 10);
        n++;
        v /= 10;
    } while (v != 0);

    while (n > 0)
    {
        n--;
        put_char(w, digits[n]);
    }
}

/* ------------------------------------------------------------- key/value */

/* 64 symbols: printable, no whitespace, no control bytes. */
static const char KEY_ALPHABET[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:";

static void fill_key(uint8_t *dst, size_t len, uint64_t *state)
{
    for (size_t i = 0; i < len; i++)
        dst[i] = (uint8_t)KEY_ALPHABET[splitmix64(state) & 63];
}

/* Random bytes with CRLF planted mid-value: the parser must skip the data
   block by its declared length, never scan it for a terminator. */
static void fill_value(uint8_t *dst, size_t len, uint64_t *state)
{
    for (size_t i = 0; i < len; i += 8)
    {
        uint64_t r = splitmix64(state);

        memcpy(dst + i, &r, min_size(8, len - i));
    }

    if (len >= 4)
    {
        dst[len / 2] = '\r';
        dst[len / 2 + 1] = '\n';
    }
}

/* -------------------------------------------------------------- commands */

typedef enum op_kind
{
    OP_GET,
    OP_SET,
    OP_DELETE,
    OP_TOUCH,
} op_kind_t;

typedef struct workload
{
    const char *name;
    op_kind_t kind;   /* used when set_pct == 0 */
    unsigned set_pct; /* > 0: mixed GET/SET stream */
    size_t key_len;
    size_t value_len;
    bool noreply;
} workload_t;

/* What the parser must report for one command. Offsets are into the stream. */
typedef struct expect
{
    command_type_t type;
    size_t start;
    size_t len; /* whole command, including data block */
    size_t key_off;
    size_t key_len;
    size_t value_off;
    size_t value_len;
    uint32_t flags;
    uint32_t exptime;
    bool noreply;
} expect_t;

static void emit_key(writer_t *w, expect_t *e, size_t key_len, uint64_t *state)
{
    e->key_off = w->pos;
    e->key_len = key_len;
    fill_key(w->buf + w->pos, key_len, state);
    w->pos += key_len;
}

static void emit_noreply(writer_t *w, expect_t *e, bool noreply)
{
    e->noreply = noreply;

    if (noreply)
        put_str(w, " noreply");
}

static void emit_get(writer_t *w, expect_t *e, const workload_t *wl, uint64_t *state)
{
    e->type = CMD_GET;
    put_str(w, "get ");
    emit_key(w, e, wl->key_len, state);
    put_str(w, "\r\n");
}

static void emit_set(writer_t *w, expect_t *e, const workload_t *wl, uint64_t *state)
{
    e->type = CMD_SET;
    e->flags = (uint32_t)(splitmix64(state) & 0xFFFF);
    e->exptime = (uint32_t)(splitmix64(state) % 86400);
    e->value_len = wl->value_len;

    put_str(w, "set ");
    emit_key(w, e, wl->key_len, state);
    put_char(w, ' ');
    put_u64(w, e->flags);
    put_char(w, ' ');
    put_u64(w, e->exptime);
    put_char(w, ' ');
    put_u64(w, e->value_len);
    emit_noreply(w, e, wl->noreply);
    put_str(w, "\r\n");

    e->value_off = w->pos;
    fill_value(w->buf + w->pos, e->value_len, state);
    w->pos += e->value_len;
    put_str(w, "\r\n");
}

static void emit_delete(writer_t *w, expect_t *e, const workload_t *wl, uint64_t *state)
{
    e->type = CMD_DELETE;
    put_str(w, "delete ");
    emit_key(w, e, wl->key_len, state);
    emit_noreply(w, e, wl->noreply);
    put_str(w, "\r\n");
}

static void emit_touch(writer_t *w, expect_t *e, const workload_t *wl, uint64_t *state)
{
    e->type = CMD_TOUCH;
    e->exptime = (uint32_t)(splitmix64(state) % 86400);

    put_str(w, "touch ");
    emit_key(w, e, wl->key_len, state);
    put_char(w, ' ');
    put_u64(w, e->exptime);
    emit_noreply(w, e, wl->noreply);
    put_str(w, "\r\n");
}

static op_kind_t pick_kind(const workload_t *wl, uint64_t *state)
{
    if (wl->set_pct == 0)
        return wl->kind;

    if (splitmix64(state) % 100 < wl->set_pct)
        return OP_SET;

    return OP_GET;
}

static void emit_one(writer_t *w, expect_t *e, const workload_t *wl, uint64_t *state)
{
    switch (pick_kind(wl, state))
    {
    case OP_GET:
        emit_get(w, e, wl, state);
        break;
    case OP_SET:
        emit_set(w, e, wl, state);
        break;
    case OP_DELETE:
        emit_delete(w, e, wl, state);
        break;
    case OP_TOUCH:
        emit_touch(w, e, wl, state);
        break;
    }
}

/* ---------------------------------------------------------------- stream */

typedef struct stream
{
    uint8_t *data;     /* what the parser sees */
    uint8_t *pristine; /* original bytes, for restore() */
    size_t len;
    size_t count;
    expect_t *expect;
} stream_t;

/* Upper bound on one command: fixed text + three numbers + key + value. */
static size_t cmd_max_len(const workload_t *wl)
{
    return 96 + wl->key_len + wl->value_len;
}

static stream_t build_stream(const workload_t *wl, size_t wanted, uint64_t seed)
{
    size_t per_cmd = cmd_max_len(wl);
    size_t count = min_size(wanted, STREAM_BUDGET / per_cmd);
    stream_t s = {0};
    writer_t w;
    uint64_t state = seed;

    s.data = xmalloc(count * per_cmd);
    s.expect = xmalloc(count * sizeof(expect_t));
    s.count = count;
    w.buf = s.data;
    w.pos = 0;

    for (size_t i = 0; i < count; i++)
    {
        expect_t *e = &s.expect[i];

        memset(e, 0, sizeof(*e));
        e->start = w.pos;
        emit_one(&w, e, wl, &state);
        e->len = w.pos - e->start;
    }

    s.len = w.pos;
    s.pristine = xmalloc(s.len);
    memcpy(s.pristine, s.data, s.len);

    return s;
}

static void free_stream(stream_t *s)
{
    free(s->data);
    free(s->pristine);
    free(s->expect);
}

/* The parser takes a mutable buffer. If it ever writes into it, every run
   must start from the original bytes. Always called outside the timer. */
static void restore(stream_t *s)
{
    memcpy(s->data, s->pristine, s->len);
}

/* ----------------------------------------------------------- correctness */

static bool bytes_equal(const uint8_t *got, const uint8_t *want, size_t len)
{
    return memcmp(got, want, len) == 0;
}

static bool check_key(const stream_t *s, const expect_t *e, const command_t *cmd)
{
    if (cmd->key_len != e->key_len)
        return false;

    return bytes_equal(cmd->key, s->pristine + e->key_off, e->key_len);
}

static bool check_value(const stream_t *s, const expect_t *e, const command_t *cmd)
{
    if (cmd->value_len != e->value_len)
        return false;

    return bytes_equal(cmd->value, s->pristine + e->value_off, e->value_len);
}

static bool check_set(const stream_t *s, const expect_t *e, const command_t *cmd)
{
    if (!check_value(s, e, cmd))
        return false;
    if (cmd->flags != e->flags)
        return false;
    if (cmd->exptime != e->exptime)
        return false;

    return cmd->noreply == e->noreply;
}

static bool check_touch(const expect_t *e, const command_t *cmd)
{
    if (cmd->exptime != e->exptime)
        return false;

    return cmd->noreply == e->noreply;
}

static bool check_fields(const stream_t *s, const expect_t *e, const command_t *cmd)
{
    if (cmd->type != e->type)
        return false;
    if (!check_key(s, e, cmd))
        return false;

    switch (e->type)
    {
    case CMD_SET:
        return check_set(s, e, cmd);
    case CMD_TOUCH:
        return check_touch(e, cmd);
    case CMD_DELETE:
        return cmd->noreply == e->noreply;
    default:
        return true;
    }
}

/* Parses the whole stream once and checks every field of every command. */
static bool verify_stream(stream_t *s)
{
    size_t pos = 0;

    restore(s);

    for (size_t i = 0; i < s->count; i++)
    {
        const expect_t *e = &s->expect[i];
        command_t cmd;
        size_t consumed = 0;
        proto_error_t err =
            protocol_parse_command(s->data + pos, s->len - pos, &cmd, &consumed);

        if (err != PROTO_OK)
        {
            fprintf(stderr, "  cmd %zu: error %d\n", i, (int)err);
            return false;
        }

        if (consumed != e->len)
        {
            fprintf(stderr, "  cmd %zu: consumed %zu, want %zu\n", i, consumed, e->len);
            return false;
        }

        if (!check_fields(s, e, &cmd))
        {
            fprintf(stderr, "  cmd %zu: parsed fields do not match\n", i);
            return false;
        }

        pos += consumed;
    }

    return true;
}

/* Every strict prefix of a valid command must be INCOMPLETE: that is what
   lets the read loop wait for more bytes instead of dropping the client. */
static bool check_prefixes(const stream_t *s)
{
    const expect_t *e = &s->expect[0];
    uint8_t *buf = xmalloc(e->len);
    bool ok = true;

    for (size_t cut = 0; cut < e->len; cut++)
    {
        command_t cmd;
        size_t consumed = 0;
        proto_error_t err;

        memcpy(buf, s->pristine + e->start, e->len);
        err = protocol_parse_command(buf, cut, &cmd, &consumed);

        if (err != PROTO_ERR_INCOMPLETE)
        {
            fprintf(stderr, "  prefix %zu of %zu bytes: error %d, want INCOMPLETE\n",
                    cut, e->len, (int)err);
            ok = false;
            break;
        }
    }

    free(buf);
    return ok;
}

static proto_error_t parse_copy(const char *src, size_t len, command_t *cmd)
{
    uint8_t buf[SCRATCH_LEN];
    size_t consumed = 0;

    memcpy(buf, src, len);
    return protocol_parse_command(buf, len, cmd, &consumed);
}

static bool is_hard_error(proto_error_t err)
{
    if (err == PROTO_OK)
        return false;
    if (err == PROTO_ERR_INCOMPLETE)
        return false;

    return true;
}

typedef struct bad_case
{
    const char *input;
    const char *what;
} bad_case_t;

/* Complete but invalid: each must be rejected, not reported INCOMPLETE,
   or a bad client stalls its connection forever. */
static const bad_case_t BAD_CASES[] = {
    {"foo k\r\n", "unknown command"},
    {"get\r\n", "get without key"},
    {"set k abc 0 5\r\nhello\r\n", "non-numeric flags"},
    {"set k 0 abc 5\r\nhello\r\n", "non-numeric exptime"},
    {"set k 0 0 abc\r\nhello\r\n", "non-numeric length"},
    {"set k 0 0 5\r\nhelloXX", "data block without CRLF"},
    {"touch k abc\r\n", "non-numeric touch exptime"},
};

static bool check_malformed(void)
{
    bool ok = true;

    for (size_t i = 0; i < sizeof(BAD_CASES) / sizeof(BAD_CASES[0]); i++)
    {
        const bad_case_t *c = &BAD_CASES[i];
        command_t cmd;
        proto_error_t err = parse_copy(c->input, strlen(c->input), &cmd);

        if (!is_hard_error(err))
        {
            fprintf(stderr, "  %s: got %d, want a hard error\n", c->what, (int)err);
            ok = false;
        }
    }

    return ok;
}

static size_t build_get(char *dst, size_t key_len)
{
    writer_t w = {(uint8_t *)dst, 0};

    put_str(&w, "get ");
    memset(w.buf + w.pos, 'k', key_len);
    w.pos += key_len;
    put_str(&w, "\r\n");

    return w.pos;
}

static bool check_key_limit(void)
{
    char buf[SCRATCH_LEN];
    command_t cmd;
    size_t len;
    bool ok = true;

    len = build_get(buf, MAX_KEY_LEN);
    if (parse_copy(buf, len, &cmd) != PROTO_OK)
    {
        fprintf(stderr, "  %d-byte key rejected\n", MAX_KEY_LEN);
        ok = false;
    }

    len = build_get(buf, MAX_KEY_LEN + 1);
    if (parse_copy(buf, len, &cmd) != PROTO_ERR_KEY_TOO_LONG)
    {
        fprintf(stderr, "  %d-byte key not reported as KEY_TOO_LONG\n", MAX_KEY_LEN + 1);
        ok = false;
    }

    return ok;
}

static bool check_simple(const char *input, command_type_t want)
{
    command_t cmd;

    if (parse_copy(input, strlen(input), &cmd) != PROTO_OK)
        return false;

    return cmd.type == want;
}

static bool check_singles(void)
{
    bool ok = true;

    if (!check_simple("version\r\n", CMD_VERSION))
    {
        fprintf(stderr, "  version not parsed\n");
        ok = false;
    }

    if (!check_simple("quit\r\n", CMD_QUIT))
    {
        fprintf(stderr, "  quit not parsed\n");
        ok = false;
    }

    return ok;
}

/* ------------------------------------------------------------ benchmarks */

static inline uint64_t consume(const command_t *cmd)
{
    return cmd->key_len + cmd->value_len + (uint64_t)cmd->type;
}

/* Whole stream available up front: pure parse cost. */
static uint64_t parse_all(uint8_t *data, size_t len, size_t *count_out)
{
    size_t pos = 0;
    size_t count = 0;
    uint64_t acc = 0;

    while (pos < len)
    {
        command_t cmd;
        size_t consumed = 0;

        if (protocol_parse_command(data + pos, len - pos, &cmd, &consumed) != PROTO_OK)
            break;

        acc += consume(&cmd);
        pos += consumed;
        count++;
    }

    *count_out = count;
    return acc;
}

static double bench_stream(const bench_config_t *cfg, stream_t *s)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        size_t count = 0;

        restore(s);

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        uint64_t acc = parse_all(s->data, s->len, &count);

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(acc);

        if (count != s->count)
            fprintf(stderr, "ERROR: parsed %zu of %zu commands\n", count, s->count);

        keep_min(&best, rep, (double)(end - start) / (double)s->count);
    }

    return best;
}

typedef struct chunk_stats
{
    size_t count;
    size_t retries; /* INCOMPLETE returns: re-parses of a partial command */
    uint64_t acc;
    bool ok;
} chunk_stats_t;

static size_t grow_window(size_t filled, size_t chunk, size_t len)
{
    return min_size(filled + chunk, len);
}

/* Bytes arrive `chunk` at a time into one contiguous buffer, as with a read
   loop that never compacts. On INCOMPLETE, the next read lands and the same
   command is parsed again from its start. */
static chunk_stats_t parse_chunked(uint8_t *data, size_t len, size_t chunk)
{
    chunk_stats_t st = {0, 0, 0, true};
    size_t pos = 0;
    size_t filled = 0;

    while (pos < len)
    {
        command_t cmd;
        size_t consumed = 0;
        proto_error_t err =
            protocol_parse_command(data + pos, filled - pos, &cmd, &consumed);

        if (err == PROTO_OK)
        {
            st.acc += consume(&cmd);
            st.count++;
            pos += consumed;
            continue;
        }

        if (err != PROTO_ERR_INCOMPLETE)
        {
            st.ok = false;
            break;
        }

        if (filled == len)
        {
            st.ok = false; /* incomplete at end of stream */
            break;
        }

        filled = grow_window(filled, chunk, len);
        st.retries++;
    }

    return st;
}

static double bench_chunked(const bench_config_t *cfg, stream_t *s, size_t chunk,
                            double *retries_per_cmd)
{
    double best = 0.0;

    for (size_t rep = 0; rep < cfg->reps; rep++)
    {
        restore(s);

        COMPILER_BARRIER();
        uint64_t start = now_ns();

        chunk_stats_t st = parse_chunked(s->data, s->len, chunk);

        uint64_t end = now_ns();
        COMPILER_BARRIER();
        sink_u64(st.acc);

        if (!st.ok)
            fprintf(stderr, "ERROR: chunked parse failed after %zu commands\n", st.count);

        *retries_per_cmd = (double)st.retries / (double)s->count;
        keep_min(&best, rep, (double)(end - start) / (double)s->count);
    }

    return best;
}

/* ------------------------------------------------------------------ runs */

static void report(const char *label, double ns_per_cmd, double bytes_per_cmd)
{
    printf("%-20s %8.2f ns/cmd | %7.2f M cmd/s | %8.0f MB/s\n",
           label, ns_per_cmd, 1e3 / ns_per_cmd, bytes_per_cmd * 1e3 / ns_per_cmd);
}

static bool run_workload(const bench_config_t *cfg, const workload_t *wl, uint64_t seed)
{
    stream_t s = build_stream(wl, cfg->commands, seed);
    bool ok = verify_stream(&s);

    if (ok)
        ok = check_prefixes(&s);

    if (ok)
    {
        double bytes_per_cmd = (double)s.len / (double)s.count;

        report(wl->name, bench_stream(cfg, &s), bytes_per_cmd);
    }
    else
    {
        fprintf(stderr, "%s: FAILED, not timed\n", wl->name);
    }

    free_stream(&s);
    return ok;
}

typedef struct chunked_run
{
    const char *name;
    workload_t workload;
    size_t chunk;
} chunked_run_t;

static void run_chunked(const bench_config_t *cfg, const chunked_run_t *run, uint64_t seed)
{
    stream_t s = build_stream(&run->workload, cfg->commands, seed);
    double retries = 0.0;
    double ns = bench_chunked(cfg, &s, run->chunk, &retries);

    report(run->name, ns, (double)s.len / (double)s.count);
    printf("%-20s %8.2f re-parses/cmd\n", "", retries);

    free_stream(&s);
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

static const workload_t WORKLOADS[] = {
    {"get k8", OP_GET, 0, 8, 0, false},
    {"get k24", OP_GET, 0, 24, 0, false},
    {"get k250", OP_GET, 0, 250, 0, false},
    {"set k24 v32", OP_SET, 0, 24, 32, false},
    {"set k24 v1K", OP_SET, 0, 24, 1024, false},
    {"set k24 v16K", OP_SET, 0, 24, 16384, false},
    {"set noreply", OP_SET, 0, 24, 32, true},
    {"delete k24", OP_DELETE, 0, 24, 0, false},
    {"touch k24", OP_TOUCH, 0, 24, 0, false},
    {"mixed 90/10", OP_GET, 10, 24, 32, false},
};

/* 1460 = one TCP MSS on a 1500 MTU link; 16384 = a typical read() size. */
static const chunked_run_t CHUNKED_RUNS[] = {
    {"mixed @1460", {"", OP_GET, 10, 24, 32, false}, 1460},
    {"mixed @16K", {"", OP_GET, 10, 24, 32, false}, 16384},
    {"set v16K @1460", {"", OP_SET, 0, 24, 16384, false}, 1460},
    {"set v16K @16K", {"", OP_SET, 0, 24, 16384, false}, 16384},
};

static bool run_checks(void)
{
    bool ok = true;

    printf("== checks ==\n");

    if (!check_malformed())
        ok = false;
    if (!check_key_limit())
        ok = false;
    if (!check_singles())
        ok = false;

    if (ok)
        printf("malformed, key limit, version/quit: ok\n");

    return ok;
}

int main(int argc, char **argv)
{
    bench_config_t cfg = {DEFAULT_COMMANDS, DEFAULT_REPS};
    bool ok;

    if (argc > 3)
    {
        fprintf(stderr, "usage: %s [commands] [reps]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2)
        cfg.commands = parse_size(argv[1], "commands");
    if (argc >= 3)
        cfg.reps = parse_size(argv[2], "reps");

    pin_to_cpu(0);

    printf("commands=%zu (capped at %lu MiB per stream) reps=%zu\n\n",
           cfg.commands, STREAM_BUDGET >> 20, cfg.reps);

    ok = run_checks();

    printf("\n== pipelined, whole buffer available (streams verified field by field) ==\n");
    for (size_t i = 0; i < sizeof(WORKLOADS) / sizeof(WORKLOADS[0]); i++)
    {
        if (!run_workload(&cfg, &WORKLOADS[i], 0x5EED0100 + i))
            ok = false;
    }

    printf("\n== chunked reads, commands split across reads ==\n");
    for (size_t i = 0; i < sizeof(CHUNKED_RUNS) / sizeof(CHUNKED_RUNS[0]); i++)
        run_chunked(&cfg, &CHUNKED_RUNS[i], 0x5EED0200 + i);

    if (!ok)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}