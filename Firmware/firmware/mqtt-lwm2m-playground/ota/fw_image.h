#ifndef FW_IMAGE_H
#define FW_IMAGE_H

/*
 * SIMULATED firmware images for the OTA learning exercise.
 *
 * Layout (little endian):
 *   0   "PGFW"           magic
 *   4   u32 total size
 *   8   char version[16] e.g. "2.0.0"
 *   24  u32 header CRC (sum of bytes 0..23, just to look like a header)
 *   28  ... deterministic pseudo-random filler up to `size`
 *
 * The integrity check the device performs is SHA-256 over the whole image,
 * compared with the manifest the server publishes. Nothing here resembles a
 * real STM32 image; it only has the right size and a verifiable digest.
 */

#include <stdint.h>
#include <string.h>

#define FW_HEADER_SIZE 28

static inline void fw_image_fill(uint8_t *buf, uint32_t size, const char *version)
{
    uint64_t x = 0x9E3779B97F4A7C15ULL ^ size;
    uint32_t sum = 0;
    for (const char *v = version; *v; v++) {
        x = x * 31 + (uint8_t)*v;
    }
    for (uint32_t i = 0; i < size; i++) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        buf[i] = (uint8_t)((x * 0x2545F4914F6CDD1DULL) >> 56);
    }
    if (size < FW_HEADER_SIZE) {
        return;
    }
    memcpy(buf, "PGFW", 4);
    buf[4] = (uint8_t)size;
    buf[5] = (uint8_t)(size >> 8);
    buf[6] = (uint8_t)(size >> 16);
    buf[7] = (uint8_t)(size >> 24);
    memset(buf + 8, 0, 16);
    strncpy((char *)buf + 8, version, 15);
    for (int i = 0; i < 24; i++) {
        sum += buf[i];
    }
    buf[24] = (uint8_t)sum;
    buf[25] = (uint8_t)(sum >> 8);
    buf[26] = (uint8_t)(sum >> 16);
    buf[27] = (uint8_t)(sum >> 24);
}

#endif /* FW_IMAGE_H */
