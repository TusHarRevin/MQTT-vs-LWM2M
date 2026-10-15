#ifndef HAL_STORAGE_H
#define HAL_STORAGE_H

#include "common/fw_types.h"

/* Persistent key/value store with integrity checking (CRC32 per record).
 * Keys: 1..HAL_STORAGE_MAX_KEY chars of [A-Za-z0-9_-]. Not thread-safe. */

#define HAL_STORAGE_MAX_KEY   15
#define HAL_STORAGE_MAX_VALUE 512

/* FW_ERR_NOT_FOUND if absent, FW_ERR_CORRUPT on bad CRC/header,
 * FW_ERR_NOMEM if the value does not fit in cap. */
int hal_storage_get(const char *key, void *buf, size_t cap, size_t *out_len);
int hal_storage_set(const char *key, const void *data, size_t len);
int hal_storage_erase(const char *key);

#endif /* HAL_STORAGE_H */
