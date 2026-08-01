#pragma once

#include <reshade.hpp>
#include <cstdint>
#include <string>

namespace TextureToolkit
{
    // Compute 32-bit CRC32 checksum over buffer
    uint32_t compute_crc32(const uint8_t *data, size_t size);

    // Format 32-bit hash into 8-character hex string (e.g., "66882833")
    std::string format_hash_hex(uint32_t hash);

    // Format 32-bit hash with 0x prefix (e.g., "0x66882833")
    std::string format_hash_hex_0x(uint32_t hash);

    // Calculate stable texture hash from description and subresource data
    uint32_t calculate_texture_hash(const reshade::api::resource_desc &desc, const reshade::api::subresource_data &data);
}
