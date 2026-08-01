#include "PNGLoader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>

namespace TextureToolkit
{
    bool load_png(const std::string &filepath, PNGImage &out_image)
    {
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load(filepath.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (pixels == nullptr)
            return false;

        out_image.width = static_cast<uint32_t>(w);
        out_image.height = static_cast<uint32_t>(h);
        out_image.channels = 4;
        out_image.pixel_data.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

        stbi_image_free(pixels);
        return true;
    }

    bool save_png(const std::string &filepath, const reshade::api::resource_desc &desc, const reshade::api::subresource_data &data)
    {
        if (data.data == nullptr || desc.texture.width == 0 || desc.texture.height == 0)
            return false;

        uint32_t w = desc.texture.width;
        uint32_t h = desc.texture.height;
        std::vector<uint8_t> rgba_buffer(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

        const uint8_t *src_bytes = static_cast<const uint8_t *>(data.data);
        uint32_t row_pitch = data.row_pitch;
        if (row_pitch == 0)
            row_pitch = reshade::api::format_row_pitch(desc.texture.format, w);

        // Convert uncompressed formats to RGBA8 for PNG output
        if (desc.texture.format == reshade::api::format::r8g8b8a8_unorm || desc.texture.format == reshade::api::format::r8g8b8a8_unorm_srgb)
        {
            for (uint32_t y = 0; y < h; ++y)
            {
                std::memcpy(rgba_buffer.data() + (y * w * 4), src_bytes + (y * row_pitch), w * 4);
            }
        }
        else if (desc.texture.format == reshade::api::format::b8g8r8a8_unorm || desc.texture.format == reshade::api::format::b8g8r8a8_unorm_srgb)
        {
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t *row_src = src_bytes + (y * row_pitch);
                uint8_t *row_dst = rgba_buffer.data() + (y * w * 4);
                for (uint32_t x = 0; x < w; ++x)
                {
                    row_dst[x * 4 + 0] = row_src[x * 4 + 2]; // Red
                    row_dst[x * 4 + 1] = row_src[x * 4 + 1]; // Green
                    row_dst[x * 4 + 2] = row_src[x * 4 + 0]; // Blue
                    row_dst[x * 4 + 3] = row_src[x * 4 + 3]; // Alpha
                }
            }
        }
        else
        {
            // For other formats (or compressed textures), write raw/unformatted pixels or copy first w*h*4 bytes
            size_t copy_size = (std::min)(rgba_buffer.size(), static_cast<size_t>(row_pitch * h));
            std::memcpy(rgba_buffer.data(), src_bytes, copy_size);
        }

        int result = stbi_write_png(filepath.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba_buffer.data(), static_cast<int>(w * 4));
        return result != 0;
    }
}
