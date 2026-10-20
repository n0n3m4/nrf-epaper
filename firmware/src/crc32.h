// Reflected CRC-32 (poly 0xEDB88320), byte-for-byte compatible with Python's
// zlib.crc32 so the client can compute the expected value with the standard
// library.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Running CRC over a stream. Seed the first call with 0.
uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len);

// One-shot convenience wrapper.
inline uint32_t crc32(const uint8_t *data, size_t len)
{
  return crc32Update(0, data, len);
}
