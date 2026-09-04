#include "hashtable.h"
#include "xxhash.h"
#include <stdbool.h>
#include <stdio.h>

int main(void)
{

    char *A = "AAAAAAAAAAA";
    char *B = "BBBBBBBBBBB";

    hashtable_t *table = hashtable_init(8, false);

    hashtable_set(table, "A", 2, A);
    hashtable_set(table, "B", 2, B);

    printf("%s\n", (char *)hashtable_get(table, "A", 2));
    printf("%s\n", (char *)hashtable_get(table, "B", 2));
    printf("%s\n", (char *)hashtable_get(table, "B", 2));

    return 0;
}