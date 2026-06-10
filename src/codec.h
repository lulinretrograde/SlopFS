/* Explicit little-endian encode/decode helpers + crc32. */
#ifndef SLOPFS_CODEC_H
#define SLOPFS_CODEC_H

#include <stdint.h>
#include <stddef.h>

static inline void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32));
}
static inline uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

uint32_t sfs_crc32(uint32_t seed, const void *data, size_t len);

#endif
