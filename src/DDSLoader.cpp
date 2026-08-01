#include "DDSLoader.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace TextureToolkit
{
#pragma pack(push, 1)
    struct DDS_PIXELFORMAT
    {
        uint32_t dwSize;
        uint32_t dwFlags;
        uint32_t dwFourCC;
        uint32_t dwRGBBitCount;
        uint32_t dwRBitMask;
        uint32_t dwGBitMask;
        uint32_t dwBBitMask;
        uint32_t dwABitMask;
    };

    struct DDS_HEADER
    {
        uint32_t dwSize;
        uint32_t dwFlags;
        uint32_t dwHeight;
        uint32_t dwWidth;
        uint32_t dwPitchOrLinearSize;
        uint32_t dwDepth;
        uint32_t dwMipMapCount;
        uint32_t dwReserved1[11];
        DDS_PIXELFORMAT ddspf;
        uint32_t dwCaps;
        uint32_t dwCaps2;
        uint32_t dwCaps3;
        uint32_t dwCaps4;
        uint32_t dwReserved2;
    };

    struct DDS_HEADER_DXT10
    {
        uint32_t dxgiFormat;
        uint32_t resourceDimension;
        uint32_t miscFlag;
        uint32_t arraySize;
        uint32_t miscFlags2;
    };
#pragma pack(pop)

    static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
    static constexpr uint32_t DDPF_FOURCC = 0x4;
    static constexpr uint32_t MAKE_FOURCC(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
    }

    static reshade::api::format dxgi_to_reshade_format(uint32_t dxgi_format)
    {
        return static_cast<reshade::api::format>(dxgi_format);
    }

    static uint32_t reshade_format_to_dxgi(reshade::api::format format)
    {
        return static_cast<uint32_t>(format);
    }

    bool load_dds(const std::string &filepath, DDSImage &out_image)
    {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = 0;
        file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
        if (magic != DDS_MAGIC)
            return false;

        DDS_HEADER header = {};
        file.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (header.dwSize != sizeof(DDS_HEADER) || header.ddspf.dwSize != sizeof(DDS_PIXELFORMAT))
            return false;

        out_image.width = header.dwWidth;
        out_image.height = header.dwHeight;
        out_image.depth = (header.dwDepth > 0) ? header.dwDepth : 1;
        out_image.mip_levels = (header.dwMipMapCount > 0) ? header.dwMipMapCount : 1;
        out_image.array_size = 1;

        reshade::api::format fmt = reshade::api::format::unknown;

        if ((header.ddspf.dwFlags & DDPF_FOURCC) != 0)
        {
            if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', '1', '0'))
            {
                DDS_HEADER_DXT10 dxt10 = {};
                file.read(reinterpret_cast<char *>(&dxt10), sizeof(dxt10));
                fmt = dxgi_to_reshade_format(dxt10.dxgiFormat);
                if (dxt10.arraySize > 0)
                    out_image.array_size = dxt10.arraySize;
            }
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', 'T', '1'))
                fmt = reshade::api::format::bc1_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', 'T', '3'))
                fmt = reshade::api::format::bc2_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('D', 'X', 'T', '5'))
                fmt = reshade::api::format::bc3_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('A', 'T', 'I', '1') || header.ddspf.dwFourCC == MAKE_FOURCC('B', 'C', '4', 'U'))
                fmt = reshade::api::format::bc4_unorm;
            else if (header.ddspf.dwFourCC == MAKE_FOURCC('A', 'T', 'I', '2') || header.ddspf.dwFourCC == MAKE_FOURCC('B', 'C', '5', 'U'))
                fmt = reshade::api::format::bc5_unorm;
        }
        else
        {
            if (header.ddspf.dwRGBBitCount == 32)
                fmt = reshade::api::format::r8g8b8a8_unorm;
            else if (header.ddspf.dwRGBBitCount == 24)
                fmt = reshade::api::format::r8g8b8a8_unorm;
        }

        if (fmt == reshade::api::format::unknown)
            fmt = reshade::api::format::r8g8b8a8_unorm;

        out_image.format = fmt;

        uint32_t current_w = out_image.width;
        uint32_t current_h = out_image.height;

        for (uint32_t mip = 0; mip < out_image.mip_levels; ++mip)
        {
            uint32_t row_pitch = reshade::api::format_row_pitch(fmt, current_w);
            uint32_t slice_pitch = reshade::api::format_slice_pitch(fmt, row_pitch, current_h);

            if (slice_pitch == 0)
                slice_pitch = row_pitch * current_h;

            std::vector<uint8_t> buffer(slice_pitch);
            file.read(reinterpret_cast<char *>(buffer.data()), slice_pitch);
            if (file.gcount() < static_cast<std::streamsize>(slice_pitch))
                break;

            out_image.subresources.push_back(std::move(buffer));
            out_image.row_pitches.push_back(row_pitch);
            out_image.slice_pitches.push_back(slice_pitch);

            current_w = (std::max)(1u, current_w / 2);
            current_h = (std::max)(1u, current_h / 2);
        }

        return !out_image.subresources.empty();
    }

    bool save_dds(const std::string &filepath, const reshade::api::resource_desc &desc, const reshade::api::subresource_data &data)
    {
        std::vector<reshade::api::subresource_data> subres;
        subres.push_back(data);
        return save_dds_multi_mip(filepath, desc, subres);
    }

    bool save_dds_multi_mip(const std::string &filepath, const reshade::api::resource_desc &desc, const std::vector<reshade::api::subresource_data> &subresources)
    {
        if (subresources.empty() || subresources[0].data == nullptr)
            return false;

        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = DDS_MAGIC;
        file.write(reinterpret_cast<const char *>(&magic), sizeof(magic));

        DDS_HEADER header = {};
        header.dwSize = sizeof(DDS_HEADER);
        header.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
        header.dwWidth = desc.texture.width;
        header.dwHeight = desc.texture.height;
        header.dwDepth = desc.texture.depth_or_layers;
        header.dwMipMapCount = static_cast<uint32_t>(subresources.size());

        if (header.dwMipMapCount > 1)
            header.dwFlags |= 0x20000; // DDSD_MIPMAPCOUNT

        header.ddspf.dwSize = sizeof(DDS_PIXELFORMAT);
        header.ddspf.dwFlags = DDPF_FOURCC;
        header.ddspf.dwFourCC = MAKE_FOURCC('D', 'X', '1', '0');
        header.dwCaps = 0x1000; // DDSCAPS_TEXTURE

        file.write(reinterpret_cast<const char *>(&header), sizeof(header));

        DDS_HEADER_DXT10 dxt10 = {};
        dxt10.dxgiFormat = reshade_format_to_dxgi(desc.texture.format);
        dxt10.resourceDimension = 3; // 2D Texture
        dxt10.arraySize = 1;

        file.write(reinterpret_cast<const char *>(&dxt10), sizeof(dxt10));

        uint32_t w = desc.texture.width;
        uint32_t h = desc.texture.height;

        for (const auto &subres : subresources)
        {
            uint32_t row_pitch = subres.row_pitch;
            if (row_pitch == 0)
                row_pitch = reshade::api::format_row_pitch(desc.texture.format, w);

            uint32_t slice_pitch = subres.slice_pitch;
            if (slice_pitch == 0)
                slice_pitch = reshade::api::format_slice_pitch(desc.texture.format, row_pitch, h);

            if (slice_pitch == 0)
                slice_pitch = row_pitch * h;

            file.write(reinterpret_cast<const char *>(subres.data), slice_pitch);

            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }

        return true;
    }
}
