#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hashtable hashtable_t;

hashtable_t *hashtable_init(size_t cap, bool free_chained_buckets);
void hashtable_destroy(hashtable_t *table);

bool hashtable_set(hashtable_t *table, const uint8_t *key, size_t key_len, void *value);
void *hashtable_get(hashtable_t *table, const uint8_t *key, size_t key_len);
void hashtable_delete(hashtable_t *table, const uint8_t *key, size_t key_len);

#endif // HASHTABLE_H
