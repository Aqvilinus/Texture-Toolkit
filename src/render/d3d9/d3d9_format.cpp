#include "render/d3d9/d3d9_format.h"
#include "texture/texture_hash.h"

#include <vector>

namespace TextureToolkit
{
    D3DFORMAT to_d3d9(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM: 
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return D3DFMT_A8R8G8B8;
        case DXGI_FORMAT_B8G8R8X8_UNORM: 
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return D3DFMT_X8R8G8B8;
        case DXGI_FORMAT_R8G8B8A8_UNORM: 
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return D3DFMT_A8R8G8B8; // Map to A8R8G8B8 and swizzle during copy
        case DXGI_FORMAT_B5G6R5_UNORM:   return D3DFMT_R5G6B5;
        case DXGI_FORMAT_B5G5R5A1_UNORM: return D3DFMT_A1R5G5B5;
        case DXGI_FORMAT_A8_UNORM:       return D3DFMT_A8;
        case DXGI_FORMAT_R8_UNORM:       return D3DFMT_L8;
        case DXGI_FORMAT_R8G8_UNORM:     return D3DFMT_A8L8;
        case DXGI_FORMAT_BC1_UNORM:      
        case DXGI_FORMAT_BC1_UNORM_SRGB:  return D3DFMT_DXT1;
        case DXGI_FORMAT_BC2_UNORM:      
        case DXGI_FORMAT_BC2_UNORM_SRGB:  return D3DFMT_DXT3;
        case DXGI_FORMAT_BC3_UNORM:      
        case DXGI_FORMAT_BC3_UNORM_SRGB:  return D3DFMT_DXT5;
        default:                                   return D3DFMT_UNKNOWN;
        }
    }

    uint32_t hash_pixels(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        const uint8_t *src = static_cast<const uint8_t *>(pixel_data);

        if (format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 || format == D3DFMT_DXT4 || format == D3DFMT_DXT5)
        {
            UINT block_size = (format == D3DFMT_DXT1) ? 8 : 16;
            UINT blocks_x = (width + 3) / 4;
            UINT blocks_y = (height + 3) / 4;
            UINT row_bytes = blocks_x * block_size;

            std::vector<uint8_t> clean_buffer;
            clean_buffer.reserve(row_bytes * blocks_y);

            for (UINT y = 0; y < blocks_y; ++y)
            {
                const uint8_t *row_src = src + y * pitch;
                clean_buffer.insert(clean_buffer.end(), row_src, row_src + row_bytes);
            }

            return compute_crc32c(clean_buffer.data(), clean_buffer.size());
        }

        UINT bpp = 4;
        if (format == D3DFMT_R5G6B5 || format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5 || format == D3DFMT_A4R4G4B4 || format == D3DFMT_L16)
        {
            bpp = 2;
        }
        else if (format == D3DFMT_L8 || format == D3DFMT_A8 || format == D3DFMT_P8)
        {
            bpp = 1;
        }

        UINT row_bytes = width * bpp;
        if (pitch == row_bytes || pitch == 0)
        {
            return compute_crc32c(src, static_cast<size_t>(pitch > 0 ? pitch : row_bytes) * height);
        }

        std::vector<uint8_t> clean_buffer;
        clean_buffer.reserve(row_bytes * height);

        for (UINT y = 0; y < height; ++y)
        {
            const uint8_t *row_src = src + y * pitch;
            clean_buffer.insert(clean_buffer.end(), row_src, row_src + row_bytes);
        }

        return compute_crc32c(clean_buffer.data(), clean_buffer.size());
    }

    DXGI_FORMAT to_dxgi(D3DFORMAT format)
    {
        switch (static_cast<uint32_t>(format))
        {
        case D3DFMT_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case D3DFMT_X8R8G8B8: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case D3DFMT_A1R5G5B5: return DXGI_FORMAT_B5G5R5A1_UNORM;
        case D3DFMT_R5G6B5:   return DXGI_FORMAT_B5G6R5_UNORM;
        case D3DFMT_A8:       return DXGI_FORMAT_A8_UNORM;
        case D3DFMT_L8:       return DXGI_FORMAT_R8_UNORM;
        case D3DFMT_A8L8:     return DXGI_FORMAT_R8G8_UNORM;
        case D3DFMT_DXT1:     return DXGI_FORMAT_BC1_UNORM;
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:     return DXGI_FORMAT_BC2_UNORM;
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:     return DXGI_FORMAT_BC3_UNORM;
        default:              return DXGI_FORMAT_UNKNOWN;
        }
    }

    std::string format_name(D3DFORMAT format)
    {
        switch (static_cast<uint32_t>(format))
        {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5:   return "R5G6B5";
        case D3DFMT_A8:       return "A8";
        case D3DFMT_L8:       return "L8";
        case D3DFMT_DXT1:     return "DXT1";
        case D3DFMT_DXT3:     return "DXT3";
        case D3DFMT_DXT5:     return "DXT5";
        default:              return "D3DFMT_" + std::to_string(static_cast<uint32_t>(format));
        }
    }
}
