#include "hashtable.h"
#include "xxhash.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HASHTABLE_BUCKET_SLOTS 8
#define HASHTABLE_SCALE_AT_FACTOR 7 / 8

int main(void)
{
    char *A = "AAAAAAAAAAA";
    char *B = "BBBBBBBBBBB";

    hashtable_t *table = hashtable_init(8);

    hashtable_set(table, (const uint8_t *)"A", 2, A);
    hashtable_set(table, (const uint8_t *)"B", 2, B);

    printf("%s\n", (char *)hashtable_get(table, (const uint8_t *)"A", 2));
    printf("%s\n", (char *)hashtable_get(table, (const uint8_t *)"B", 2));
    printf("%s\n", (char *)hashtable_get(table, (const uint8_t *)"B", 2));

    return 0;
}