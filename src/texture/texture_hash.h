#pragma once

#include <cstdint>
#include <string>

namespace TextureToolkit
{
    // CRC-32C over the buffer. This value names every replacement file on disk.
    uint32_t compute_crc32c(const uint8_t *data, size_t size);

    // The same value as hashing the rows laid end to end, without building that buffer. Row padding
    // is the driver's to choose and can be uninitialised, so hashing it would make one texture hash
    // differently on another machine -- and would not match what Special K names its packs with.
    uint32_t compute_crc32c_rows(const uint8_t *data, size_t row_pitch, size_t row_bytes, size_t rows);

    // Eight uppercase hex digits, the form replacement files are named in.
    std::string format_hash_hex(uint32_t hash);
}
