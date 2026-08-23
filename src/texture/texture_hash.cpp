#include "texture/texture_hash.h"
#include <cstring>
#include <vector>

#include <array>
#include <nmmintrin.h>
#include <windows.h>

namespace TextureToolkit
{
    // CRC-32C (Castagnoli, reflected polynomial 0x82F63B78). This value names every replacement
    // file, so it must never drift. Castagnoli because SSE4.2 implements it in hardware, and
    // because Special K names its packs the same way, so those load here unchanged.
    //
    // Not the google/crc32c library: measured 6.3 GB/s against this code's 14.1 on x86.
    //
    // Three-stream layout, zeros operator and block sizes after Mark Adler's crc32c.c (zlib
    // license), which Special K's implementation descends from as well; names follow his.
    namespace
    {
        constexpr uint32_t kPolynomial = 0x82F63B78u;
        constexpr uint32_t kSeed = 0xFFFFFFFFu;

        using ByteTable = std::array<uint32_t, 256>;
        using ZerosTable = std::array<std::array<uint32_t, 256>, 4>;
        using Gf2Matrix = std::array<uint32_t, 32>;

        constexpr ByteTable make_byte_table()
        {
            ByteTable table{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t remainder = i;
                for (int bit = 0; bit < 8; ++bit)
                    remainder = (remainder & 1) ? (kPolynomial ^ (remainder >> 1)) : (remainder >> 1);
                table[i] = remainder;
            }
            return table;
        }

        constexpr ByteTable kByteTable = make_byte_table();

        // char rather than uint8_t so the check value below can be computed at compile time:
        // reinterpret_cast is not allowed in a constant expression, a per-byte static_cast is.
        constexpr uint32_t crc32c_sw(uint32_t crc, const char *data, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
                crc = (crc >> 8) ^ kByteTable[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF];
            return crc;
        }

        // The published check value for CRC-32C, so a broken table cannot ship.
        static_assert(~crc32c_sw(kSeed, "123456789", 9) == 0xE3069283u, "CRC-32C table is wrong");

        bool has_hardware_crc32()
        {
            static const bool supported =
                IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE) != FALSE;
            return supported;
        }

        // The crc32 instruction has a three-cycle latency, so one chained stream reaches about a
        // third of the core. Three interleaved streams keep it fed; joining them means advancing a
        // CRC as if N zero bytes followed it, a matrix-vector product in GF(2) folded into byte
        // tables so the hot path does four lookups.

        uint32_t gf2_matrix_times(const Gf2Matrix &mat, uint32_t vec)
        {
            uint32_t sum = 0;
            for (int i = 0; vec != 0; ++i, vec >>= 1)
                if (vec & 1)
                    sum ^= mat[i];
            return sum;
        }

        Gf2Matrix gf2_matrix_square(const Gf2Matrix &mat)
        {
            Gf2Matrix square{};
            for (int i = 0; i < 32; ++i)
                square[i] = gf2_matrix_times(mat, mat[i]);
            return square;
        }

        // The operator that applies `len` zero bytes to a CRC. len must be a power of two.
        Gf2Matrix crc32c_zeros_op(size_t len)
        {
            Gf2Matrix op{};
            op[0] = kPolynomial;
            for (int i = 1; i < 32; ++i)
                op[i] = 1u << (i - 1);

            op = gf2_matrix_square(op);   // two bits
            op = gf2_matrix_square(op);   // four bits
            op = gf2_matrix_square(op);   // one byte

            for (size_t n = 1; n < len; n <<= 1)
                op = gf2_matrix_square(op);

            return op;
        }

        ZerosTable crc32c_zeros(size_t len)
        {
            const Gf2Matrix op = crc32c_zeros_op(len);

            ZerosTable zeros{};
            for (int byte = 0; byte < 4; ++byte)
                for (uint32_t value = 0; value < 256; ++value)
                    zeros[byte][value] = gf2_matrix_times(op, value << (8 * byte));
            return zeros;
        }

        uint32_t crc32c_shift(const ZerosTable &zeros, uint32_t crc)
        {
            return zeros[0][crc & 0xFF] ^
                   zeros[1][(crc >> 8) & 0xFF] ^
                   zeros[2][(crc >> 16) & 0xFF] ^
                   zeros[3][(crc >> 24) & 0xFF];
        }

        // Two block sizes so merely-large buffers still interleave: a small texture's mip is a few
        // kilobytes and would run single-stream against the long block alone.
        constexpr size_t kLong = 8192;
        constexpr size_t kShort = 256;

        // Interleaved rather than three sequential passes, which would leave the dependency chain
        // as long as one stream.
        uint32_t crc32c_three_streams(uint32_t crc, const uint8_t *data, size_t len, const ZerosTable &zeros)
        {
#ifdef _M_X64
            uint64_t a = crc, b = 0, c = 0;
            for (size_t i = 0; i < len; i += 8)
            {
                a = _mm_crc32_u64(a, *reinterpret_cast<const uint64_t *>(data + i));
                b = _mm_crc32_u64(b, *reinterpret_cast<const uint64_t *>(data + len + i));
                c = _mm_crc32_u64(c, *reinterpret_cast<const uint64_t *>(data + 2 * len + i));
            }
#else
            uint32_t a = crc, b = 0, c = 0;
            for (size_t i = 0; i < len; i += 4)
            {
                a = _mm_crc32_u32(a, *reinterpret_cast<const uint32_t *>(data + i));
                b = _mm_crc32_u32(b, *reinterpret_cast<const uint32_t *>(data + len + i));
                c = _mm_crc32_u32(c, *reinterpret_cast<const uint32_t *>(data + 2 * len + i));
            }
#endif
            crc = crc32c_shift(zeros, static_cast<uint32_t>(a)) ^ static_cast<uint32_t>(b);
            return crc32c_shift(zeros, crc) ^ static_cast<uint32_t>(c);
        }

        uint32_t crc32c_hw(uint32_t crc, const uint8_t *data, size_t size)
        {
            static const ZerosTable crc32c_long = crc32c_zeros(kLong);
            static const ZerosTable crc32c_short = crc32c_zeros(kShort);

            while (size >= 3 * kLong)
            {
                crc = crc32c_three_streams(crc, data, kLong, crc32c_long);
                data += 3 * kLong;
                size -= 3 * kLong;
            }

            while (size >= 3 * kShort)
            {
                crc = crc32c_three_streams(crc, data, kShort, crc32c_short);
                data += 3 * kShort;
                size -= 3 * kShort;
            }

#ifdef _M_X64
            uint64_t wide = crc;
            while (size >= 8)
            {
                wide = _mm_crc32_u64(wide, *reinterpret_cast<const uint64_t *>(data));
                data += 8;
                size -= 8;
            }
            crc = static_cast<uint32_t>(wide);
#endif
            while (size >= 4)
            {
                crc = _mm_crc32_u32(crc, *reinterpret_cast<const uint32_t *>(data));
                data += 4;
                size -= 4;
            }
            while (size-- != 0)
                crc = _mm_crc32_u8(crc, *data++);

            return crc;
        }
    }

    uint32_t compute_crc32c(const uint8_t *data, size_t size)
    {
        if (has_hardware_crc32())
            return ~crc32c_hw(kSeed, data, size);

        return ~crc32c_sw(kSeed, reinterpret_cast<const char *>(data), size);
    }

    uint32_t compute_crc32c_rows(const uint8_t *data, size_t row_pitch, size_t row_bytes, size_t rows)
    {
        if (data == nullptr || row_bytes == 0 || rows == 0)
            return 0;

        if (row_pitch == 0 || row_pitch == row_bytes)
            return compute_crc32c(data, row_bytes * rows);

        // Two ways to skip the padding, and which is faster depends on size. Below the threshold
        // the rows are gathered and hashed in one call, because a call per row is too short to
        // reach the three-stream path and runs at about half rate; above it the gather costs more
        // memory bandwidth than it saves. Measured crossover is around a megabyte: at 16 MB the
        // row walk does 24.5 GB/s against 14.1 for the gather, at 128x128 it is 12.9 against 24.5.
        constexpr size_t kGatherLimit = 1u << 20;

        const size_t tight_size = row_bytes * rows;
        if (tight_size <= kGatherLimit)
        {
            // Reused rather than allocated per call, and per thread because a game may upload from
            // several at once.
            thread_local std::vector<uint8_t> gathered;
            gathered.resize(tight_size);
            for (size_t y = 0; y < rows; ++y)
                std::memcpy(gathered.data() + y * row_bytes, data + y * row_pitch, row_bytes);

            return compute_crc32c(gathered.data(), tight_size);
        }

        // Chained without inverting between rows, so the result equals a hash of the tight rows
        // laid end to end -- the same value Special K arrives at, and the same one the gather above
        // produces.
        const bool hardware = has_hardware_crc32();
        uint32_t crc = kSeed;
        for (size_t y = 0; y < rows; ++y)
        {
            const uint8_t *row = data + y * row_pitch;
            crc = hardware ? crc32c_hw(crc, row, row_bytes)
                           : crc32c_sw(crc, reinterpret_cast<const char *>(row), row_bytes);
        }

        return ~crc;
    }

    std::string format_hash_hex(uint32_t hash)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out(8, '0');
        for (int i = 7; i >= 0; --i, hash >>= 4)
            out[i] = digits[hash & 0xF];
        return out;
    }
}
