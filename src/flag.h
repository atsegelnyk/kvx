#ifndef FLAG_H
#define FLAG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum flag_err_t
{
    FLAG_OK,
    FLAG_ERR_FLAG_MALFORMED,
    FLAG_ERR_UNKNOWN_FLAG,
    FLAG_ERR_MISSING_VALUE,
    FLAG_ERR_INVALID_VALUE,
} flag_err_t;

void flag_i64(const char *short_name, const char *long_name, int64_t *dst, const char *usage);
void flag_u64(const char *short_name, const char *long_name, uint64_t *dst, const char *usage);
void flag_bool(const char *short_name, const char *long_name, bool *dst, const char *usage);
void flag_str(const char *short_name, const char *long_name, const char **dst, const char *usage);

flag_err_t flag_parse(int argc, char **argv);

#endif