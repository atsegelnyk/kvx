#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kv.h"
#include "xxhash.h"

#include <stdio.h>

int main(void)
{

    kv_t *kv = kv_init();

    uint8_t buf[100];
    size_t buf_len = 100;

    size_t value_len;

    uint16_t flags;
    uint32_t exp;

    kv_set(kv, (const uint8_t *)"A", 2, (const uint8_t *)"AAAAA", 6, 1, 999999999);
    kv_get(kv, (const uint8_t *)"A", 2, buf, buf_len, &value_len, &flags, &exp);
    printf("key: %s  flags: %d exp: %d\n", buf, flags, exp);

    kv_set(kv, (const uint8_t *)"A", 2, (const uint8_t *)"BBBBB", 6, 1, 999999999);
    kv_get(kv, (const uint8_t *)"A", 2, buf, buf_len, &value_len, &flags, &exp);
    printf("key: %s  flags: %d exp: %d\n", buf, flags, exp);

    kv_set(kv, (const uint8_t *)"A", 2, (const uint8_t *)"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC", 67, 1, 999999999);
    kv_get(kv, (const uint8_t *)"A", 2, buf, buf_len, &value_len, &flags, &exp);
    printf("key: %s  flags: %d exp: %d\n", buf, flags, exp);

    return 0;
}
