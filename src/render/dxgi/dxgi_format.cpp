#include "render/dxgi/dxgi_format.h"
#include "core/logger.h"

#include <DirectXTex.h>
#include <algorithm>
#include <bit>

#include "texture/texture_hash.h"

namespace TextureToolkit
{
    bool tight_rows(DXGI_FORMAT fmt, uint32_t width, uint32_t height, size_t &row_bytes, size_t &rows)
    {
        size_t slice = 0;
        if (FAILED(DirectX::ComputePitch(fmt, width, height, row_bytes, slice)) || row_bytes == 0)
            return false;

        rows = DirectX::ComputeScanlines(fmt, height);
        return rows != 0;
    }

    uint32_t texture_byte_size(DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t mips)
    {
        uint32_t total = 0;
        for (uint32_t m = 0; m < (mips == 0 ? 1u : mips); ++m)
        {
            size_t row = 0, slice = 0;
            if (SUCCEEDED(DirectX::ComputePitch(fmt, w, h, row, slice)))
                total += static_cast<uint32_t>(slice);
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }
        return total;
    }

    uint32_t full_mip_count(uint32_t w, uint32_t h)
    {
        return static_cast<uint32_t>(std::bit_width((std::max)({w, h, 1u})));
    }

    bool load_dds(const std::filesystem::path &path, bool generate_chain,
                  DirectX::ScratchImage &image, DirectX::TexMetadata &meta)
    {
        if (FAILED(DirectX::LoadFromDDSFile(path.wstring().c_str(), DirectX::DDS_FLAGS_NONE, &meta, image)))
            return false;

        // Only the first slice of the first item is read below. Saying so out loud beats silently
        // dropping five sixths of a cube map, which is what happened before.
        if (meta.arraySize > 1 || meta.depth > 1 || meta.IsCubemap())
        {
            Logger::get().warn("[Textures] " + path.filename().string() +
                               " is a texture array, cube map or volume texture; only the first slice is used.");
        }

        if (generate_chain && meta.mipLevels == 1)
        {
            DirectX::ScratchImage mipped;
            if (SUCCEEDED(DirectX::GenerateMipMaps(*image.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, mipped)))
            {
                image = std::move(mipped);
                meta = image.GetMetadata();
            }
        }

        return true;
    }
    // Falls back to the numeric id for formats games do not ship textures in.
    std::string format_name(DXGI_FORMAT f)
    {
        // The library has no name table, and Special K writes the same switch by hand -- 115 cases
        // in their case. One token per format keeps the string and the enumerator from drifting.
#define NAME(x) case DXGI_FORMAT_##x: return #x;
        switch (f)
        {
            NAME(R8G8B8A8_UNORM)
            NAME(R8G8B8A8_UNORM_SRGB)
            NAME(R8G8B8A8_TYPELESS)
            NAME(B8G8R8A8_UNORM)
            NAME(B8G8R8A8_UNORM_SRGB)
            NAME(B8G8R8X8_UNORM)
            NAME(R10G10B10A2_UNORM)
            NAME(R16G16B16A16_FLOAT)
            NAME(R8_UNORM)
            NAME(R8G8_UNORM)
            NAME(A8_UNORM)
            NAME(BC1_UNORM)
            NAME(BC1_UNORM_SRGB)
            NAME(BC1_TYPELESS)
            NAME(BC2_UNORM)
            NAME(BC2_UNORM_SRGB)
            NAME(BC3_UNORM)
            NAME(BC3_UNORM_SRGB)
            NAME(BC3_TYPELESS)
            NAME(BC4_UNORM)
            NAME(BC4_SNORM)
            NAME(BC5_UNORM)
            NAME(BC5_SNORM)
            NAME(BC6H_UF16)
            NAME(BC6H_SF16)
            NAME(BC7_UNORM)
            NAME(BC7_UNORM_SRGB)
            NAME(BC7_TYPELESS)
        default:
            return std::to_string(static_cast<uint32_t>(f));
        }
#undef NAME
    }
    // A view and a .dds file both need a concrete format, and most DDS tools cannot read
    // them. Map them to the matching UNORM view format. Concrete formats, including the
    // _SRGB variants, pass through unchanged so sRGB intent is preserved.
    DXGI_FORMAT resolve_typeless(DXGI_FORMAT f)
    {
        // BC6H is the one TYPELESS format the library's UNORM mapping does not cover: it is
        // float-only, and its FLOAT mapping does not know about block formats.
        if (f == DXGI_FORMAT_BC6H_TYPELESS)
            return DXGI_FORMAT_BC6H_UF16;

        const DXGI_FORMAT unorm = DirectX::MakeTypelessUNORM(f);
        return unorm != f ? unorm : DirectX::MakeTypelessFLOAT(f);
    }
}
