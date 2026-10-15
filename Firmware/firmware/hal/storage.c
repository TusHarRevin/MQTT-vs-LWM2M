#include "hal/storage.h"

#include <string.h>

#include "common/fw_log.h"
#include "platform/port.h"

#define TAG "storage"

#define STORAGE_MAGIC   0x46575354u /* "FWST" */
#define STORAGE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint32_t crc;
} record_hdr_t;

/* Static to keep large buffers off small MCU stacks. */
static uint8_t s_record[sizeof(record_hdr_t) + HAL_STORAGE_MAX_VALUE];

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;

    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static bool key_valid(const char *key)
{
    size_t n;

    if (key == NULL) {
        return false;
    }
    n = strlen(key);
    if (n == 0 || n > HAL_STORAGE_MAX_KEY) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = key[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

int hal_storage_get(const char *key, void *buf, size_t cap, size_t *out_len)
{
    record_hdr_t hdr;
    size_t n = 0;
    int rc;

    if (!key_valid(key) || (buf == NULL && cap > 0)) {
        return FW_ERR_PARAM;
    }
    rc = port_storage_read(key, s_record, sizeof(s_record), &n);
    if (rc != FW_OK) {
        return rc;
    }
    if (n < sizeof(hdr)) {
        LOG_W(TAG, "'%s': truncated record", key);
        return FW_ERR_CORRUPT;
    }
    memcpy(&hdr, s_record, sizeof(hdr));
    if (hdr.magic != STORAGE_MAGIC || hdr.version != STORAGE_VERSION ||
        hdr.len != n - sizeof(hdr) ||
        hdr.crc != crc32(s_record + sizeof(hdr), hdr.len)) {
        LOG_W(TAG, "'%s': corrupt record", key);
        return FW_ERR_CORRUPT;
    }
    if (hdr.len > cap) {
        return FW_ERR_NOMEM;
    }
    memcpy(buf, s_record + sizeof(hdr), hdr.len);
    if (out_len) {
        *out_len = hdr.len;
    }
    return FW_OK;
}

int hal_storage_set(const char *key, const void *data, size_t len)
{
    record_hdr_t hdr;

    if (!key_valid(key) || len > HAL_STORAGE_MAX_VALUE || (data == NULL && len > 0)) {
        return FW_ERR_PARAM;
    }
    hdr.magic = STORAGE_MAGIC;
    hdr.version = STORAGE_VERSION;
    hdr.len = (uint16_t)len;
    hdr.crc = crc32(data, len);
    memcpy(s_record, &hdr, sizeof(hdr));
    if (len > 0) {
        memcpy(s_record + sizeof(hdr), data, len);
    }
    return port_storage_write(key, s_record, sizeof(hdr) + len);
}

int hal_storage_erase(const char *key)
{
    if (!key_valid(key)) {
        return FW_ERR_PARAM;
    }
    return port_storage_erase(key);
}
