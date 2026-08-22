#pragma once

#include <cstdint>
#include <string>

namespace TextureToolkit
{
    // CRC-32C over the buffer. This value names every replacement file on disk.
    uint32_t compute_crc32c(const uint8_t *data, size_t size);

    // Eight uppercase hex digits, the form replacement files are named in.
    std::string format_hash_hex(uint32_t hash);
}
