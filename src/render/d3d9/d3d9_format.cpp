#include "render/d3d9/d3d9_format.h"
#include "texture/texture_hash.h"

#include "core/logger.h"
#include <unordered_set>

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

    // Bytes per pixel, or 0 for a format we do not know. Zero matters: the old code assumed four
    // for anything unlisted, so a one- or two-byte format made the row four times too long and the
    // copy below read past the end of the locked rectangle.
    static UINT bytes_per_pixel(D3DFORMAT format)
    {
        switch (format)
        {
        case D3DFMT_A8:
        case D3DFMT_P8:
        case D3DFMT_L8:
        case D3DFMT_A4L4:
        case D3DFMT_R3G3B2:
            return 1;

        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8R3G3B2:
        case D3DFMT_A8P8:
        case D3DFMT_A8L8:
        case D3DFMT_V8U8:
        case D3DFMT_L6V5U5:
        case D3DFMT_CxV8U8:
        case D3DFMT_L16:
        case D3DFMT_R16F:
        case D3DFMT_D16:
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D15S1:
        case D3DFMT_INDEX16:
            return 2;

        case D3DFMT_R8G8B8:
            return 3;

        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_G16R16:
        case D3DFMT_X8L8V8U8:
        case D3DFMT_Q8W8V8U8:
        case D3DFMT_V16U16:
        case D3DFMT_A2W10V10U10:
        case D3DFMT_G16R16F:
        case D3DFMT_R32F:
        case D3DFMT_D32:
        case D3DFMT_D32F_LOCKABLE:
        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
        case D3DFMT_D24X4S4:
        case D3DFMT_D24FS8:
        case D3DFMT_INDEX32:
            return 4;

        case D3DFMT_A16B16G16R16:
        case D3DFMT_Q16W16V16U16:
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_G32R32F:
            return 8;

        case D3DFMT_A32B32G32R32F:
            return 16;

        default:
            return 0;
        }
    }

    // Bytes per 4x4 block, or 0 when the format is not block-compressed. ATI1 and ATI2 are FourCC
    // codes rather than enumerators, and were falling through to the four-bytes-per-pixel path.
    static UINT block_bytes(D3DFORMAT format)
    {
        switch (static_cast<DWORD>(format))
        {
        case D3DFMT_DXT1:
        case MAKEFOURCC('A', 'T', 'I', '1'):
        case MAKEFOURCC('B', 'C', '4', 'U'):
        case MAKEFOURCC('B', 'C', '4', 'S'):
            return 8;

        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
        case MAKEFOURCC('A', 'T', 'I', '2'):
        case MAKEFOURCC('B', 'C', '5', 'U'):
        case MAKEFOURCC('B', 'C', '5', 'S'):
            return 16;

        default:
            return 0;
        }
    }

    bool is_block_compressed(D3DFORMAT format)
    {
        return block_bytes(format) != 0;
    }

    uint32_t hash_pixels(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        UINT rows = height;
        UINT row_bytes = 0;

        if (const UINT block = block_bytes(format))
        {
            rows = (height + 3) / 4;
            row_bytes = ((width + 3) / 4) * block;
        }
        else if (const UINT bpp = bytes_per_pixel(format))
        {
            row_bytes = width * bpp;
        }
        else
        {
            // Guessing a size here is a read past the end of the locked rectangle. A texture in a
            // format we cannot measure is left untracked instead; the caller says so, once.
            return 0;
        }

        // The same call the D3D11 side makes: rows only, no copy, and row padding -- the driver's
        // to choose and undefined where it chose it -- left out, as Special K leaves it out.
        return compute_crc32c_rows(static_cast<const uint8_t *>(pixel_data), pitch, row_bytes, rows);
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
