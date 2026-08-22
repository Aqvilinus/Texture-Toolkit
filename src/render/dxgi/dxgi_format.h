#pragma once

#include <dxgi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace DirectX { class ScratchImage; struct TexMetadata; struct Image; }

namespace TextureToolkit
{
    uint32_t compute_slice_pitch(DXGI_FORMAT fmt, uint32_t row_pitch, uint32_t height);
    uint32_t texture_byte_size(DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t mips);
    uint32_t full_mip_count(uint32_t w, uint32_t h);

    std::string format_name(DXGI_FORMAT f);
    DXGI_FORMAT resolve_typeless(DXGI_FORMAT f);

    // Set `generate_chain` when the original was mipmapped and the file may not be. The levels are
    // read back from `image` itself -- GetImage(level, 0, 0) -- rather than copied into a parallel
    // list of pointers into the same pixels.
    bool load_dds(const std::filesystem::path &path, bool generate_chain,
                  DirectX::ScratchImage &image, DirectX::TexMetadata &meta);
}
