#ifndef KV_H
#define KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum kv_error_t
{
    KV_OK,
    KV_ERR_NOT_FOUND,
    KV_ERR_NO_MEM,
    KV_ERR_VALUE_LEN_EXCEEDS_DST_BUFFER_SIZE,
} kv_error_t;

typedef struct kv_t kv_t;

kv_t *kv_init(void);
void kv_destroy(kv_t *kv);

kv_error_t kv_get(kv_t *kv,
                  const uint8_t *key,
                  size_t key_len,
                  uint8_t *dst,
                  size_t dst_len,
                  size_t *value_len_out,
                  uint16_t *flags_out,
                  uint32_t *exp_out);

kv_error_t kv_set(kv_t *kv,
                  const uint8_t *key,
                  size_t key_len,
                  const uint8_t *value,
                  size_t value_len,
                  uint16_t flags,
                  uint32_t exp);

kv_error_t kv_delete(kv_t *kv,
                     const uint8_t *key,
                     size_t key_len);

#endif // KV_H
