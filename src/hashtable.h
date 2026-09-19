#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "item.h"

typedef struct hashtable hashtable_t;

hashtable_t *hashtable_init(size_t cap);
void hashtable_destroy(hashtable_t *table);

item_t **hashtable_set(hashtable_t *table, uint64_t hash, const uint8_t *key, size_t key_len);
item_t *hashtable_get(hashtable_t *table, uint64_t hash, const uint8_t *key, size_t key_len);
item_t *hashtable_delete(hashtable_t *table, uint64_t hash, const uint8_t *key, size_t key_len);

size_t hashtable_len(const hashtable_t *table);
size_t hashtable_bucket_count(const hashtable_t *table);
int hashtable_migrating(const hashtable_t *table);

#endif // HASHTABLE_H
