#pragma once

#include <reshade.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace TextureToolkit
{
    struct PNGImage
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 4;
        std::vector<uint8_t> pixel_data; // RGBA8
    };

    bool load_png(const std::string &filepath, PNGImage &out_image);
    bool save_png(const std::string &filepath, const reshade::api::resource_desc &desc, const reshade::api::subresource_data &data);
}
