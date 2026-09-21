#include "flag.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FLAG_MAX
#define FLAG_MAX 64
#endif

typedef enum flag_type_t
{
    T_I64,
    T_U64,
    T_BOOL,
    T_STR,
} flag_type_t;

typedef struct flag_t
{
    flag_type_t type;
    const char *short_name;
    const char *long_name;
    const char *usage;
    void *dst;
} flag_t;

typedef struct arg_t
{
    bool is_long;
    const char *name;
    size_t name_len;
    const char *value;
} arg_t;

static flag_t flags[FLAG_MAX];
static size_t nflags;

static void bind(flag_type_t type, const char *short_name, const char *long_name, void *dst, const char *usage)
{
    if (nflags == FLAG_MAX)
        abort();

    flags[nflags] = (flag_t){
        .type = type,
        .short_name = short_name,
        .long_name = long_name,
        .usage = usage,
        .dst = dst,
    };
    nflags++;
}

void flag_i64(const char *short_name, const char *long_name, int64_t *dst, const char *usage)
{
    bind(T_I64, short_name, long_name, dst, usage);
}

void flag_u64(const char *short_name, const char *long_name, uint64_t *dst, const char *usage)
{
    bind(T_U64, short_name, long_name, dst, usage);
}

void flag_bool(const char *short_name, const char *long_name, bool *dst, const char *usage)
{
    bind(T_BOOL, short_name, long_name, dst, usage);
}

void flag_str(const char *short_name, const char *long_name, const char **dst, const char *usage)
{
    bind(T_STR, short_name, long_name, dst, usage);
}

static flag_err_t parse_i64(const char *v, int64_t *dst)
{
    char *end;

    errno = 0;
    intmax_t x = strtoimax(v, &end, 10);

    if (end == v || *end != '\0')
        return FLAG_ERR_INVALID_VALUE;

    if (errno == ERANGE)
        return FLAG_ERR_INVALID_VALUE;

    if (x < INT64_MIN || x > INT64_MAX)
        return FLAG_ERR_INVALID_VALUE;

    *dst = (int64_t)x;
    return FLAG_OK;
}

static flag_err_t parse_u64(const char *v, uint64_t *dst)
{
    char *end;

    /* strtoumax skips whitespace and accepts '-' (wrapping the result) */
    if (!isdigit((unsigned char)*v))
        return FLAG_ERR_INVALID_VALUE;

    errno = 0;
    uintmax_t x = strtoumax(v, &end, 10);

    if (*end != '\0')
        return FLAG_ERR_INVALID_VALUE;

    if (errno == ERANGE || x > UINT64_MAX)
        return FLAG_ERR_INVALID_VALUE;

    *dst = (uint64_t)x;
    return FLAG_OK;
}

static flag_err_t parse_bool(const char *v, bool *dst)
{
    if (strcmp(v, "true") == 0)
    {
        *dst = true;
        return FLAG_OK;
    }

    if (strcmp(v, "false") == 0)
    {
        *dst = false;
        return FLAG_OK;
    }

    return FLAG_ERR_INVALID_VALUE;
}

static flag_err_t set_value(const flag_t *f, const char *v)
{
    switch (f->type)
    {
    case T_I64:
        return parse_i64(v, f->dst);
    case T_U64:
        return parse_u64(v, f->dst);
    case T_BOOL:
        return parse_bool(v, f->dst);
    case T_STR:
        *(const char **)f->dst = v;
        return FLAG_OK;
    }

    return FLAG_ERR_INVALID_VALUE;
}

static flag_err_t split_arg(const char *raw, arg_t *out)
{
    size_t dashes = strspn(raw, "-");
    if (dashes != 1 && dashes != 2)
        return FLAG_ERR_FLAG_MALFORMED;

    out->is_long = (dashes == 2);
    out->name = raw + dashes;
    out->value = NULL;

    const char *eq = strchr(out->name, '=');
    if (eq)
    {
        out->name_len = (size_t)(eq - out->name);
        out->value = eq + 1;
    }
    else
    {
        out->name_len = strlen(out->name);
    }

    if (out->name_len == 0)
        return FLAG_ERR_FLAG_MALFORMED;

    return FLAG_OK;
}

static bool name_matches(const char *declared, const arg_t *arg)
{
    if (!declared)
        return false;

    if (strncmp(declared, arg->name, arg->name_len) != 0)
        return false;

    return declared[arg->name_len] == '\0';
}

static const flag_t *lookup(const arg_t *arg)
{
    for (size_t i = 0; i < nflags; i++)
    {
        const char *declared = flags[i].short_name;
        if (arg->is_long)
            declared = flags[i].long_name;

        if (name_matches(declared, arg))
            return &flags[i];
    }

    return NULL;
}

static flag_err_t consume_flag(int argc, char **argv, int *i)
{
    arg_t arg;
    flag_err_t err = split_arg(argv[*i], &arg);
    if (err != FLAG_OK)
        return err;
    (*i)++;

    const flag_t *f = lookup(&arg);
    if (!f)
        return FLAG_ERR_UNKNOWN_FLAG;

    if (arg.value)
        return set_value(f, arg.value);

    if (f->type == T_BOOL)
        return set_value(f, "true");

    if (*i >= argc)
        return FLAG_ERR_MISSING_VALUE;

    const char *value = argv[*i];
    (*i)++;

    return set_value(f, value);
}

static const char *type_name(flag_type_t type)
{
    switch (type)
    {
    case T_I64:
        return "int64";
    case T_U64:
        return "uint64";
    case T_STR:
        return "string";
    case T_BOOL:
        return NULL;
    }

    return NULL;
}

static const char *err_string(flag_err_t err)
{
    switch (err)
    {
    case FLAG_OK:
        return "ok";
    case FLAG_ERR_FLAG_MALFORMED:
        return "malformed flag";
    case FLAG_ERR_UNKNOWN_FLAG:
        return "unknown flag";
    case FLAG_ERR_MISSING_VALUE:
        return "missing value for flag";
    case FLAG_ERR_INVALID_VALUE:
        return "invalid value for flag";
    }

    return "unknown error";
}

static void print_names(FILE *out, const flag_t *f)
{
    if (f->short_name)
        fprintf(out, "-%s", f->short_name);

    if (f->short_name && f->long_name)
        fprintf(out, ", ");

    if (f->long_name)
        fprintf(out, "--%s", f->long_name);
}

static void print_flag(FILE *out, const flag_t *f)
{
    fprintf(out, "  ");
    print_names(out, f);

    const char *type = type_name(f->type);
    if (type)
        fprintf(out, " <%s>", type);

    fprintf(out, "\n");

    if (f->usage)
        fprintf(out, "        %s\n", f->usage);
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out, "Usage: %s [flags]\n\nFlags:\n", prog);

    for (size_t i = 0; i < nflags; i++)
        print_flag(out, &flags[i]);
}

static void report_error(const char *prog, const char *arg, flag_err_t err)
{
    fprintf(stderr, "%s: %s: %s\n\n", prog, err_string(err), arg);
    print_usage(stderr, prog);
}

flag_err_t flag_parse(int argc, char **argv)
{
    int i = 1;

    while (i < argc)
    {
        int start = i;

        flag_err_t err = consume_flag(argc, argv, &i);
        if (err != FLAG_OK)
        {
            report_error(argv[0], argv[start], err);
            return err;
        }
    }

    return FLAG_OK;
}
