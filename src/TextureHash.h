#pragma once

#include <cstdint>
#include <string>

namespace TextureToolkit
{
    // Compute 32-bit CRC32 checksum over buffer
    uint32_t compute_crc32(const uint8_t *data, size_t size);

    // Format 32-bit hash into 8-character hex string (e.g., "66882833")
    std::string format_hash_hex(uint32_t hash);
}
