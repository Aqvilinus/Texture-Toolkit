#include "TextureManager.h"
#include "Config.h"
#include "D3D9Hook.h"
#include "D3D11Hook.h"
#include "DDSLoader.h"
#include "Logger.h"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace TextureToolkit
{
    // Private-data GUID used to tag every original game resource with its content
    // hash. The driver clears private data when the object is destroyed, so a reused
    // pointer belonging to a new resource never carries a stale hash -- this is what
    // makes replacement lookups immune to driver pointer reuse.
    // {6B7A4C10-3F2E-4D9A-9E21-8C0A5B1D2E34}
    static const GUID TT_HASH_GUID =
        { 0x6b7a4c10, 0x3f2e, 0x4d9a, { 0x9e, 0x21, 0x8c, 0x0a, 0x5b, 0x1d, 0x2e, 0x34 } };

    static bool is_block_compressed(reshade::api::format format);
    static D3DFORMAT dxgi_to_d3d9_format(reshade::api::format format);

    // Bytes per pixel for the uncompressed formats we can box-downsample. 0 = not supported.
    static uint32_t uncompressed_bpp(reshade::api::format f)
    {
        switch (f)
        {
        case reshade::api::format::r8g8b8a8_unorm:
        case reshade::api::format::r8g8b8a8_unorm_srgb:
        case reshade::api::format::b8g8r8a8_unorm:
        case reshade::api::format::b8g8r8a8_unorm_srgb:
        case reshade::api::format::b8g8r8x8_unorm:
        case reshade::api::format::b8g8r8x8_unorm_srgb:
            return 4;
        case reshade::api::format::r8g8_unorm: return 2;
        case reshade::api::format::r8_unorm:
        case reshade::api::format::a8_unorm:  return 1;
        default: return 0;
        }
    }

    // Total GPU byte size of a texture across all its mip levels.
    static uint32_t compute_texture_bytes(reshade::api::format fmt, uint32_t w, uint32_t h, uint32_t mips)
    {
        uint32_t total = 0;
        for (uint32_t m = 0; m < (mips == 0 ? 1u : mips); ++m)
        {
            uint32_t rp = reshade::api::format_row_pitch(fmt, w);
            uint32_t sp = reshade::api::format_slice_pitch(fmt, rp, h);
            if (sp == 0)
                sp = rp * h;
            total += sp;
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }
        return total;
    }

    // Number of mip levels in a full chain down to 1x1 for the given dimensions.
    static uint32_t full_mip_count(uint32_t w, uint32_t h)
    {
        uint32_t levels = 1;
        while (w > 1 || h > 1)
        {
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
            ++levels;
        }
        return levels;
    }

    // 2x2 box-filter downsample of a tightly-addressed uncompressed image.
    static std::vector<uint8_t> downsample_2x(const uint8_t *src, uint32_t src_w, uint32_t src_h, uint32_t src_pitch, uint32_t bpp)
    {
        uint32_t dw = (std::max)(1u, src_w / 2);
        uint32_t dh = (std::max)(1u, src_h / 2);
        std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * bpp);

        for (uint32_t y = 0; y < dh; ++y)
        {
            uint32_t sy0 = y * 2;
            uint32_t sy1 = (std::min)(sy0 + 1, src_h - 1);
            const uint8_t *r0 = src + static_cast<size_t>(sy0) * src_pitch;
            const uint8_t *r1 = src + static_cast<size_t>(sy1) * src_pitch;
            uint8_t *drow = dst.data() + static_cast<size_t>(y) * dw * bpp;

            for (uint32_t x = 0; x < dw; ++x)
            {
                uint32_t sx0 = (x * 2) * bpp;
                uint32_t sx1 = ((std::min)(x * 2 + 1, src_w - 1)) * bpp;
                for (uint32_t c = 0; c < bpp; ++c)
                {
                    uint32_t sum = r0[sx0 + c] + r0[sx1 + c] + r1[sx0 + c] + r1[sx1 + c];
                    drow[x * bpp + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        return dst;
    }

    // One resolved mip level ready to upload: 'ptr' points either into the DDS payload
    // (for levels the file supplies) or into 'data' (for auto-generated levels).
    struct MipLevel
    {
        std::vector<uint8_t> data;
        const uint8_t *ptr = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t row_pitch = 0;
        uint32_t slice_pitch = 0;
    };

    // Resolve levels [0, target_levels) for a replacement of dds.width x dds.height.
    // Uses DDS subresources where present; auto-generates the rest for uncompressed
    // formats. For compressed formats missing a level, the chain stops early (a shorter
    // but fully-valid chain) rather than leaving an uninitialised level.
    static std::vector<MipLevel> build_replacement_mips(const DDSImage &dds, uint32_t target_levels)
    {
        std::vector<MipLevel> levels;
        const uint32_t bpp = uncompressed_bpp(dds.format);
        const bool compressed = is_block_compressed(dds.format);

        uint32_t w = dds.width, h = dds.height;
        for (uint32_t i = 0; i < target_levels; ++i)
        {
            MipLevel lvl;
            lvl.width = w;
            lvl.height = h;

            if (i < dds.subresources.size())
            {
                lvl.ptr = dds.subresources[i].data();
                lvl.row_pitch = dds.row_pitches[i];
                lvl.slice_pitch = dds.slice_pitches[i];
            }
            else
            {
                // Auto-generate this level by downsampling the previous one.
                if (compressed || bpp == 0 || levels.empty())
                    break; // Cannot synthesise; keep the shorter valid chain.

                const MipLevel &prev = levels.back();
                lvl.data = downsample_2x(prev.ptr, prev.width, prev.height, prev.row_pitch, bpp);
                lvl.ptr = lvl.data.data();
                lvl.row_pitch = w * bpp;
                lvl.slice_pitch = static_cast<uint32_t>(lvl.data.size());
            }

            levels.push_back(std::move(lvl));
            w = (std::max)(1u, w / 2);
            h = (std::max)(1u, h / 2);
        }
        return levels;
    }

    static uint32_t calculate_d3d9_pixel_hash(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        const uint8_t *src = static_cast<const uint8_t *>(pixel_data);

        // Check if format is DXT compressed
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

            return compute_crc32(clean_buffer.data(), clean_buffer.size());
        }

        // Uncompressed Formats: calculate bytes per pixel
        UINT bpp = 4; // Default 32-bit RGBA/ARGB
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
            return compute_crc32(src, static_cast<size_t>(pitch > 0 ? pitch : row_bytes) * height);
        }

        std::vector<uint8_t> clean_buffer;
        clean_buffer.reserve(row_bytes * height);

        for (UINT y = 0; y < height; ++y)
        {
            const uint8_t *row_src = src + y * pitch;
            clean_buffer.insert(clean_buffer.end(), row_src, row_src + row_bytes);
        }

        return compute_crc32(clean_buffer.data(), clean_buffer.size());
    }

    // Human-readable DXGI_FORMAT name. Covers the formats games actually ship textures in;
    // anything else falls back to the numeric id.
    static std::string dxgi_format_name(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM:        return "B8G8R8X8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8_UNORM:              return "R8_UNORM";
        case DXGI_FORMAT_R8G8_UNORM:            return "R8G8_UNORM";
        case DXGI_FORMAT_A8_UNORM:              return "A8_UNORM";
        case DXGI_FORMAT_BC1_UNORM:             return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB:        return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC1_TYPELESS:          return "BC1_TYPELESS";
        case DXGI_FORMAT_BC2_UNORM:             return "BC2_UNORM";
        case DXGI_FORMAT_BC2_UNORM_SRGB:        return "BC2_UNORM_SRGB";
        case DXGI_FORMAT_BC3_UNORM:             return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB:        return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC3_TYPELESS:          return "BC3_TYPELESS";
        case DXGI_FORMAT_BC4_UNORM:             return "BC4_UNORM";
        case DXGI_FORMAT_BC4_SNORM:             return "BC4_SNORM";
        case DXGI_FORMAT_BC5_UNORM:             return "BC5_UNORM";
        case DXGI_FORMAT_BC5_SNORM:             return "BC5_SNORM";
        case DXGI_FORMAT_BC6H_UF16:             return "BC6H_UF16";
        case DXGI_FORMAT_BC6H_SF16:             return "BC6H_SF16";
        case DXGI_FORMAT_BC7_UNORM:             return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB:        return "BC7_UNORM_SRGB";
        case DXGI_FORMAT_BC7_TYPELESS:          return "BC7_TYPELESS";
        default:                                return std::to_string(static_cast<uint32_t>(f));
        }
    }

    static bool dxgi_format_is_compressed(DXGI_FORMAT f)
    {
        return f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM
            || f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB;
    }

    // TYPELESS formats cannot back a shader resource view, and most DDS tools cannot read
    // them. Map them to the matching UNORM view format. Concrete formats, including the
    // _SRGB variants, pass through unchanged so sRGB intent is preserved.
    static DXGI_FORMAT dxgi_concrete_format(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_BC1_TYPELESS:          return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:          return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:          return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS:          return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS:          return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS:         return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS:          return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UNORM;
        case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;
        default:                                return f;
        }
    }

    static bool dxgi_format_is_srgb(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    TextureManager &TextureManager::get()
    {
        static TextureManager instance;
        return instance;
    }

    void TextureManager::init()
    {
        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));

        m_game_dir = std::filesystem::path(exe_path).parent_path();

        // Load settings from config
        const auto& cfg = ConfigManager::get().get_config();

        // Resolve dump/inject under the resource root. A relative root (the default, "TT") is
        // taken relative to the game directory; an absolute root is used as-is (operator/
        // returns the right-hand path when it is absolute).
        std::filesystem::path root = m_game_dir / cfg.resource_root;
        m_dump_dir = root / "dump";
        m_inject_dir = root / "inject";

        auto_dump = cfg.auto_dump;
        enable_injection = cfg.enable_injection;
        filter_small_textures = cfg.filter_small_textures;
        show_current_frame_only = cfg.show_current_frame_only;

        std::error_code ec;
        std::filesystem::create_directories(m_dump_dir, ec);
        std::filesystem::create_directories(m_inject_dir, ec);

        Logger::get().info("[TextureManager] Standalone Texture Toolkit initialized.");
        Logger::get().info("[TextureManager] Dump directory: " + m_dump_dir.string());
        Logger::get().info("[TextureManager] Inject directory: " + m_inject_dir.string());

        rescan_injected();

        // Start background dump worker
        m_dump_thread_running = true;
        m_dump_thread = std::thread(&TextureManager::dump_worker_loop, this);
    }

    void TextureManager::shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_dump_mutex);
            m_dump_thread_running = false;
        }
        m_dump_cv.notify_one();
        if (m_dump_thread.joinable())
        {
            m_dump_thread.join();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        release_replacements();
        release_preview();

        for (auto &rb : m_readback_queue)
        {
            if (rb.tex9) rb.tex9->Release();
            if (rb.srv11) rb.srv11->Release();
        }
        m_readback_queue.clear();
    }

    void TextureManager::set_preview_target(uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (hash == m_preview_target_hash)
            return;
        release_preview();
        m_preview_target_hash = hash;
    }

    uint64_t TextureManager::get_original_preview_handle()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_preview_srv11 != nullptr)
            return reinterpret_cast<uint64_t>(m_preview_srv11);
        if (m_preview_tex9 != nullptr)
            return reinterpret_cast<uint64_t>(m_preview_tex9);
        return 0;
    }

    // Releases the pinned original preview and the cached file preview. Caller MUST hold m_mutex.
    void TextureManager::release_preview()
    {
        if (m_preview_tex9 != nullptr)
        {
            m_preview_tex9->Release();
            m_preview_tex9 = nullptr;
        }
        if (m_preview_srv11 != nullptr)
        {
            m_preview_srv11->Release();
            m_preview_srv11 = nullptr;
        }
        if (m_file_preview_tex9 != nullptr)
        {
            m_file_preview_tex9->Release();
            m_file_preview_tex9 = nullptr;
        }
        if (m_file_preview_srv11 != nullptr)
        {
            m_file_preview_srv11->Release();
            m_file_preview_srv11 = nullptr;
        }
        m_file_preview_hash = 0;
    }

    uint64_t TextureManager::get_file_preview_handle(uint32_t hash, const std::string &dds_path, bool is_dx11)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (hash == m_file_preview_hash)
            return m_file_preview_srv11 ? reinterpret_cast<uint64_t>(m_file_preview_srv11)
                                        : reinterpret_cast<uint64_t>(m_file_preview_tex9);

        // Drop any previously cached file preview.
        if (m_file_preview_tex9)  { m_file_preview_tex9->Release();  m_file_preview_tex9 = nullptr; }
        if (m_file_preview_srv11) { m_file_preview_srv11->Release(); m_file_preview_srv11 = nullptr; }
        m_file_preview_hash = hash;

        DDSImage dds;
        if (!load_dds(dds_path, dds) || dds.subresources.empty())
            return 0;

        if (is_dx11)
        {
            ID3D11Device *dev = D3D11Hook::get().get_device();
            if (dev == nullptr)
                return 0;

            DXGI_FORMAT view_fmt = dxgi_concrete_format(static_cast<DXGI_FORMAT>(dds.format));

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = dds.width;
            desc.Height = dds.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = view_fmt;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sd = {};
            sd.pSysMem = dds.subresources[0].data();
            sd.SysMemPitch = dds.row_pitches[0];
            sd.SysMemSlicePitch = dds.slice_pitches[0];

            ID3D11Texture2D *tex = nullptr;
            D3D11Hook::s_inside_injection = true;
            HRESULT hr = dev->CreateTexture2D(&desc, &sd, &tex);
            if (SUCCEEDED(hr) && tex != nullptr)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
                svd.Format = desc.Format;
                svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                svd.Texture2D.MipLevels = 1;
                hr = dev->CreateShaderResourceView(tex, &svd, &m_file_preview_srv11);
                tex->Release();
            }
            D3D11Hook::s_inside_injection = false;
            return reinterpret_cast<uint64_t>(m_file_preview_srv11);
        }

        // DX9
        IDirect3DDevice9 *dev = D3D9Hook::get().get_device();
        if (dev == nullptr)
            return 0;

        D3DFORMAT fmt = dxgi_to_d3d9_format(dds.format);
        if (fmt == D3DFMT_UNKNOWN)
            return 0;

        IDirect3DTexture9 *tex = nullptr;
        D3D9Hook::s_inside_injection = true;
        if (SUCCEEDED(dev->CreateTexture(dds.width, dds.height, 1, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr)) && tex != nullptr)
        {
            D3DLOCKED_RECT r = {};
            if (SUCCEEDED(tex->LockRect(0, &r, nullptr, 0)))
            {
                bool needs_swizzle = (dds.format == reshade::api::format::r8g8b8a8_unorm || dds.format == reshade::api::format::r8g8b8a8_unorm_srgb);
                UINT block_h = is_block_compressed(dds.format) ? 4 : 1;
                UINT rows = (dds.height + block_h - 1) / block_h;
                UINT src_pitch = dds.row_pitches[0];
                const uint8_t *src = dds.subresources[0].data();
                uint8_t *dst = static_cast<uint8_t *>(r.pBits);

                for (UINT y = 0; y < rows; ++y)
                {
                    if (needs_swizzle)
                    {
                        const uint8_t *sr = src + y * src_pitch;
                        uint8_t *dr = dst + y * r.Pitch;
                        for (UINT x = 0; x < dds.width; ++x)
                        {
                            dr[x * 4 + 0] = sr[x * 4 + 2];
                            dr[x * 4 + 1] = sr[x * 4 + 1];
                            dr[x * 4 + 2] = sr[x * 4 + 0];
                            dr[x * 4 + 3] = sr[x * 4 + 3];
                        }
                    }
                    else
                    {
                        std::memcpy(dst + y * r.Pitch, src + y * src_pitch, (std::min)(static_cast<UINT>(r.Pitch), src_pitch));
                    }
                }
                tex->UnlockRect(0);
                m_file_preview_tex9 = tex;
            }
            else
            {
                tex->Release();
            }
        }
        D3D9Hook::s_inside_injection = false;
        return reinterpret_cast<uint64_t>(m_file_preview_tex9);
    }

    // Releases the COM reference we hold for every stored replacement. Caller MUST
    // already hold m_mutex (the mutex is non-recursive).
    void TextureManager::release_replacements()
    {
        for (auto &p : m_d3d9_replacements)
        {
            if (p.second != nullptr)
                p.second->Release();
        }
        m_d3d9_replacements.clear();

        for (auto &p : m_d3d11_replacements)
        {
            if (p.second != nullptr)
                p.second->Release();
        }
        m_d3d11_replacements.clear();
    }

    void TextureManager::on_frame()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame_count++;

        m_active_frame_hashes = m_current_frame_hashes;
        m_current_frame_hashes.clear();

        for (uint32_t hash : m_active_frame_hashes)
        {
            auto it = m_tracked_textures.find(hash);
            if (it != m_tracked_textures.end())
                it->second.last_seen_frame = m_frame_count;
        }

        // Trim the tracked list occasionally (every ~5s at 60 fps) so it stays bounded.
        if (m_frame_count % 300 == 0)
            evict_stale_textures();

        process_readback_queue();
    }

    void TextureManager::evict_stale_textures()
    {
        // Remove textures not drawn for a while. Keep anything with a loaded replacement,
        // the texture currently selected for preview, and any hash that has an inject file,
        // so injected and selected textures never disappear from the panel.
        constexpr uint64_t kEvictAgeFrames = 3600; // ~1 minute at 60 fps
        if (m_frame_count < kEvictAgeFrames)
            return;

        for (auto it = m_tracked_textures.begin(); it != m_tracked_textures.end();)
        {
            const TextureDetails &d = it->second;
            bool stale = d.last_seen_frame + kEvictAgeFrames < m_frame_count;
            bool keep = d.replacement_handle != 0 ||
                        it->first == m_preview_target_hash ||
                        m_injected_files.find(it->first) != m_injected_files.end();

            if (stale && !keep)
                it = m_tracked_textures.erase(it);
            else
                ++it;
        }
    }

    void TextureManager::rescan_injected()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        release_replacements();
        m_injected_files.clear();

        if (!std::filesystem::exists(m_inject_dir))
            return;

        for (const auto &entry : std::filesystem::directory_iterator(m_inject_dir))
        {
            if (!entry.is_regular_file())
                continue;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            // DDS-only injection for maximum format/compatibility safety.
            if (ext != ".dds")
                continue;

            std::string stem = entry.path().stem().string();
            if (stem.rfind("0x", 0) == 0 || stem.rfind("0X", 0) == 0)
                stem = stem.substr(2);

            try
            {
                uint32_t hash = static_cast<uint32_t>(std::stoul(stem, nullptr, 16));
                m_injected_files[hash] = entry.path();
            }
            catch (...)
            {
                // Ignore non-hex filenames
            }
        }

        Logger::get().info("[TextureManager] Scanned " + std::to_string(m_injected_files.size()) + " DDS replacement file(s) in TT/inject.");
    }

    std::filesystem::path TextureManager::find_injection_path(uint32_t hash)
    {
        auto it = m_injected_files.find(hash);
        if (it != m_injected_files.end())
        {
            return it->second;
        }
        return std::filesystem::path();
    }

    void TextureManager::copy_tag9(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
    {
        if (src == nullptr || dst == nullptr)
            return;

        uint32_t hash = 0;
        DWORD size = sizeof(hash);
        if (SUCCEEDED(src->GetPrivateData(TT_HASH_GUID, &hash, &size)) && size == sizeof(hash))
            dst->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);
    }

    IDirect3DBaseTexture9 *TextureManager::get_replacement_texture9(IDirect3DBaseTexture9 *orig)
    {
        if (orig == nullptr)
            return orig;

        // Resolve the texture's content hash from its private-data tag. Untracked
        // textures carry no tag, so this fails fast for the common case.
        uint32_t hash = 0;
        DWORD size = sizeof(hash);
        if (FAILED(orig->GetPrivateData(TT_HASH_GUID, &hash, &size)) || size != sizeof(hash))
            return orig;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_frame_hashes.insert(hash);

        // Pin the live original for preview if this is the selected texture.
        if (hash == m_preview_target_hash && m_preview_tex9 == nullptr)
        {
            orig->AddRef();
            m_preview_tex9 = orig;
        }

        // Bulk dump: take a reference the first time a queued texture is drawn.
        if (!m_pending_dumps.empty())
        {
            auto pit = m_pending_dumps.find(hash);
            if (pit != m_pending_dumps.end())
            {
                m_pending_dumps.erase(pit);
                orig->AddRef();
                m_readback_queue.push_back({hash, orig, nullptr});
            }
        }

        if (!enable_injection)
            return orig;

        auto it = m_d3d9_replacements.find(hash);
        if (it != m_d3d9_replacements.end() && it->second != nullptr)
            return it->second;

        return orig;
    }

    static DXGI_FORMAT d3d9_format_to_dxgi(D3DFORMAT format)
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

    static std::string d3d9_format_to_string(D3DFORMAT format)
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

    static bool is_block_compressed(reshade::api::format format)
    {
        switch (format)
        {
        case reshade::api::format::bc1_unorm:
        case reshade::api::format::bc1_unorm_srgb:
        case reshade::api::format::bc2_unorm:
        case reshade::api::format::bc2_unorm_srgb:
        case reshade::api::format::bc3_unorm:
        case reshade::api::format::bc3_unorm_srgb:
        case reshade::api::format::bc4_unorm:
        case reshade::api::format::bc4_snorm:
        case reshade::api::format::bc5_unorm:
        case reshade::api::format::bc5_snorm:
        case reshade::api::format::bc6h_ufloat:
        case reshade::api::format::bc6h_sfloat:
        case reshade::api::format::bc7_unorm:
        case reshade::api::format::bc7_unorm_srgb:
            return true;
        default:
            return false;
        }
    }

    static D3DFORMAT dxgi_to_d3d9_format(reshade::api::format format)
    {
        switch (format)
        {
        case reshade::api::format::b8g8r8a8_unorm: 
        case reshade::api::format::b8g8r8a8_unorm_srgb: return D3DFMT_A8R8G8B8;
        case reshade::api::format::b8g8r8x8_unorm: 
        case reshade::api::format::b8g8r8x8_unorm_srgb: return D3DFMT_X8R8G8B8;
        case reshade::api::format::r8g8b8a8_unorm: 
        case reshade::api::format::r8g8b8a8_unorm_srgb: return D3DFMT_A8R8G8B8; // Map to A8R8G8B8 and swizzle during copy
        case reshade::api::format::b5g6r5_unorm:   return D3DFMT_R5G6B5;
        case reshade::api::format::b5g5r5a1_unorm: return D3DFMT_A1R5G5B5;
        case reshade::api::format::a8_unorm:       return D3DFMT_A8;
        case reshade::api::format::r8_unorm:       return D3DFMT_L8;
        case reshade::api::format::r8g8_unorm:     return D3DFMT_A8L8;
        case reshade::api::format::bc1_unorm:      
        case reshade::api::format::bc1_unorm_srgb:  return D3DFMT_DXT1;
        case reshade::api::format::bc2_unorm:      
        case reshade::api::format::bc2_unorm_srgb:  return D3DFMT_DXT3;
        case reshade::api::format::bc3_unorm:      
        case reshade::api::format::bc3_unorm_srgb:  return D3DFMT_DXT5;
        default:                                   return D3DFMT_UNKNOWN;
        }
    }

    void TextureManager::register_unmap_texture9(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch)
    {
        if (device == nullptr || texture == nullptr || pixel_data == nullptr || width == 0 || height == 0)
            return;

        if (filter_small_textures && (width < 16 || height < 16))
            return;

        uint32_t hash = calculate_d3d9_pixel_hash(pixel_data, width, height, format, pitch);
        if (hash == 0)
            return;

        UINT original_levels = texture->GetLevelCount();

        std::lock_guard<std::mutex> lock(m_mutex);

        // Tag the original resource with its content hash. Used both for active-scene
        // tracking and for resolving replacements at bind time (SetTexture), and is
        // immune to driver pointer reuse.
        texture->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);

        bool dx9_compressed = (format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 ||
                               format == D3DFMT_DXT4 || format == D3DFMT_DXT5);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.width = width;
        details.height = height;
        details.mip_levels = original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "D3DFMT_" + d3d9_format_to_string(format);
        details.format_short = "D3D9_" + d3d9_format_to_string(format);
        details.is_compressed = dx9_compressed;
        details.is_srgb = false; // D3D9 sRGB is a sampler state, not part of the format
        details.is_dx11 = false;
        {
            DXGI_FORMAT ddx = d3d9_format_to_dxgi(format);
            details.data_size = (ddx != DXGI_FORMAT_UNKNOWN)
                ? compute_texture_bytes(static_cast<reshade::api::format>(ddx), width, height, original_levels)
                : width * height * 4;
        }
        details.last_seen_frame = m_frame_count;

        std::filesystem::path inject_path = find_injection_path(hash);
        if (enable_injection && !inject_path.empty() &&
            m_d3d9_replacements.find(hash) == m_d3d9_replacements.end())
        {
            DDSImage dds;
            if (load_dds(inject_path.string(), dds) && !dds.subresources.empty())
            {
                D3DFORMAT d3d9_target_fmt = dxgi_to_d3d9_format(dds.format);
                if (d3d9_target_fmt != D3DFMT_UNKNOWN)
                {
                    // Match the original's mip topology: single level stays single,
                    // a mipmapped original gets a full chain (auto-generated as needed).
                    uint32_t target_levels = (original_levels <= 1) ? 1u : full_mip_count(dds.width, dds.height);
                    std::vector<MipLevel> mips = build_replacement_mips(dds, target_levels);

                    if (mips.empty())
                    {
                        Logger::get().error("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " produced no usable mip levels.");
                    }
                    else
                    {
                        if (original_levels > 1 && mips.size() < target_levels)
                        {
                            Logger::get().error("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " is missing mip levels (" + std::to_string(mips.size()) + "/" + std::to_string(target_levels) + "). Compressed replacements must ship a full mip chain; re-export with mipmaps to avoid shimmering at distance.");
                        }

                        IDirect3DTexture9 *highres_tex = nullptr;
                        D3D9Hook::s_inside_injection = true;
                        HRESULT hr = device->CreateTexture(
                            dds.width, dds.height, static_cast<UINT>(mips.size()), 0,
                            d3d9_target_fmt, D3DPOOL_MANAGED, &highres_tex, nullptr);

                        bool needs_swizzle = (dds.format == reshade::api::format::r8g8b8a8_unorm || dds.format == reshade::api::format::r8g8b8a8_unorm_srgb);
                        UINT block_height = is_block_compressed(dds.format) ? 4 : 1;

                        if (SUCCEEDED(hr) && highres_tex != nullptr)
                        {
                            bool upload_ok = true;
                            for (size_t lvl = 0; lvl < mips.size(); ++lvl)
                            {
                                D3DLOCKED_RECT rect = {};
                                if (FAILED(highres_tex->LockRect(static_cast<UINT>(lvl), &rect, nullptr, 0)))
                                {
                                    upload_ok = false;
                                    break;
                                }

                                const MipLevel &m = mips[lvl];
                                UINT num_rows = (m.height + (block_height - 1)) / block_height;
                                UINT copy_row_pitch = (std::min)(static_cast<UINT>(rect.Pitch), m.row_pitch);
                                uint8_t *dest_ptr = static_cast<uint8_t *>(rect.pBits);

                                if (needs_swizzle)
                                {
                                    for (UINT y = 0; y < num_rows; ++y)
                                    {
                                        const uint8_t *src_row = m.ptr + y * m.row_pitch;
                                        uint8_t *dest_row = dest_ptr + y * rect.Pitch;
                                        for (UINT x = 0; x < m.width; ++x)
                                        {
                                            uint8_t r = src_row[x * 4 + 0];
                                            uint8_t g = src_row[x * 4 + 1];
                                            uint8_t b = src_row[x * 4 + 2];
                                            uint8_t a = src_row[x * 4 + 3];
                                            dest_row[x * 4 + 0] = b;
                                            dest_row[x * 4 + 1] = g;
                                            dest_row[x * 4 + 2] = r;
                                            dest_row[x * 4 + 3] = a;
                                        }
                                    }
                                }
                                else
                                {
                                    for (UINT y = 0; y < num_rows; ++y)
                                        std::memcpy(dest_ptr + y * rect.Pitch, m.ptr + y * m.row_pitch, copy_row_pitch);
                                }

                                highres_tex->UnlockRect(static_cast<UINT>(lvl));
                            }

                            if (upload_ok)
                            {
                                // Keep our reference (released in release_replacements).
                                m_d3d9_replacements[hash] = highres_tex;

                                details.status = TextureStatus::INJECTED;
                                details.filepath_injected = inject_path.string();
                                details.replacement_handle = reinterpret_cast<uint64_t>(highres_tex);
                                details.repl_width = dds.width;
                                details.repl_height = dds.height;

                                Logger::get().info("[TextureManager] Loaded high-res DX9 replacement for 0x" + format_hash_hex(hash) + " (" + std::to_string(dds.width) + "x" + std::to_string(dds.height) + ", " + std::to_string(mips.size()) + " mips, original had " + std::to_string(original_levels) + ")");
                            }
                            else
                            {
                                highres_tex->Release();
                                Logger::get().error("[TextureManager] Failed to upload mip data for DX9 replacement 0x" + format_hash_hex(hash));
                            }
                        }
                        else
                        {
                            Logger::get().error("[TextureManager] Failed to create high-res replacement D3D9 texture for 0x" + format_hash_hex(hash));
                        }
                        D3D9Hook::s_inside_injection = false;
                    }
                }
                else
                {
                    Logger::get().error("[TextureManager] Unsupported DX9 format mapping for injected texture " + inject_path.string() + ", format ID: " + std::to_string(static_cast<uint32_t>(dds.format)));
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to load injected DDS file " + inject_path.string());
            }
        }

        m_tracked_textures[hash] = details;
        Logger::get().debug("[TextureManager] Tracked D3D9 texture: 0x" + details.hash_hex + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

        if (auto_dump)
        {
            DXGI_FORMAT dxgi_fmt = d3d9_format_to_dxgi(format);
            if (dxgi_fmt != DXGI_FORMAT_UNKNOWN)
            {
                dump_texture(hash, width, height, dxgi_fmt, pixel_data, pitch);
                m_tracked_textures[hash].status = TextureStatus::DUMPED;
            }
        }
    }

    ID3D11ShaderResourceView *TextureManager::get_replacement_srv11(ID3D11ShaderResourceView *orig)
    {
        if (orig == nullptr)
            return orig;

        ID3D11Resource *orig_res = nullptr;
        orig->GetResource(&orig_res);
        if (orig_res == nullptr)
            return orig;

        // Resolve the content hash from the resource's private-data tag.
        uint32_t hash = 0;
        UINT size = sizeof(hash);
        HRESULT hr = orig_res->GetPrivateData(TT_HASH_GUID, &size, &hash);
        orig_res->Release();
        if (FAILED(hr) || size != sizeof(hash))
            return orig;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_current_frame_hashes.insert(hash);

        // Record how the game samples this texture (the SRV's concrete view format). Done
        // once per texture; it reveals the sRGB intent that a TYPELESS resource hides.
        {
            auto tit = m_tracked_textures.find(hash);
            if (tit != m_tracked_textures.end() && tit->second.view_format_id == 0)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
                orig->GetDesc(&vd);
                if (vd.Format != DXGI_FORMAT_UNKNOWN)
                {
                    tit->second.view_format_id = static_cast<uint32_t>(vd.Format);
                    tit->second.view_format_str = dxgi_format_name(vd.Format);
                }
            }
        }

        // Pin the live original SRV for preview if this is the selected texture.
        if (hash == m_preview_target_hash && m_preview_srv11 == nullptr)
        {
            orig->AddRef();
            m_preview_srv11 = orig;
        }

        // Bulk dump: take a reference the first time a queued texture is drawn.
        if (!m_pending_dumps.empty())
        {
            auto pit = m_pending_dumps.find(hash);
            if (pit != m_pending_dumps.end())
            {
                m_pending_dumps.erase(pit);
                orig->AddRef();
                m_readback_queue.push_back({hash, nullptr, orig});
            }
        }

        if (!enable_injection)
            return orig;

        auto it = m_d3d11_replacements.find(hash);
        if (it != m_d3d11_replacements.end() && it->second != nullptr)
            return it->second;

        return orig;
    }

    void TextureManager::register_unmap_texture11(ID3D11Device *device, ID3D11Resource *resource, const void *pixel_data, UINT width, UINT height, DXGI_FORMAT format, UINT pitch)
    {
        if (device == nullptr || resource == nullptr || pixel_data == nullptr || width == 0 || height == 0)
            return;

        if (filter_small_textures && (width < 16 || height < 16))
            return;

        reshade::api::format reshade_fmt = static_cast<reshade::api::format>(format);
        UINT slice_pitch = reshade::api::format_slice_pitch(reshade_fmt, pitch, height);
        if (slice_pitch == 0)
            return;

        uint32_t hash = compute_crc32(static_cast<const uint8_t *>(pixel_data), slice_pitch);
        if (hash == 0)
            return;

        // Original description drives the replacement's mip topology and the info panel.
        D3D11_TEXTURE2D_DESC orig_desc = {};
        {
            ID3D11Texture2D *orig_tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&orig_tex))) && orig_tex != nullptr)
            {
                orig_tex->GetDesc(&orig_desc);
                orig_tex->Release();
            }
        }
        UINT original_levels = orig_desc.MipLevels; // 0 = full chain generated by the runtime

        std::lock_guard<std::mutex> lock(m_mutex);

        // Tag the original resource with its content hash (see get_replacement_srv11).
        resource->SetPrivateData(TT_HASH_GUID, sizeof(hash), &hash);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.width = width;
        details.height = height;
        details.mip_levels = (original_levels == 0) ? full_mip_count(width, height) : original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "DXGI_FORMAT_" + dxgi_format_name(format);
        details.format_short = "DX11_" + dxgi_format_name(format);
        details.is_compressed = dxgi_format_is_compressed(format);
        details.is_srgb = dxgi_format_is_srgb(format);
        details.is_dx11 = true;
        details.array_size = (orig_desc.ArraySize > 0) ? orig_desc.ArraySize : 1;
        details.bind_flags = orig_desc.BindFlags;
        details.misc_flags = orig_desc.MiscFlags;
        details.cpu_access = orig_desc.CPUAccessFlags;
        details.usage = static_cast<uint32_t>(orig_desc.Usage);
        details.data_size = compute_texture_bytes(reshade_fmt, width, height, details.mip_levels);
        details.last_seen_frame = m_frame_count;

        std::filesystem::path inject_path = find_injection_path(hash);
        if (enable_injection && !inject_path.empty() &&
            m_d3d11_replacements.find(hash) == m_d3d11_replacements.end())
        {
            DDSImage dds;
            if (load_dds(inject_path.string(), dds) && !dds.subresources.empty())
            {
                // Single-level originals stay single-level; mipmapped originals
                // (or runtime-generated full chains, MipLevels == 0) get a full chain.
                uint32_t target_levels = (original_levels == 1) ? 1u : full_mip_count(dds.width, dds.height);
                std::vector<MipLevel> mips = build_replacement_mips(dds, target_levels);

                if (mips.empty())
                {
                    Logger::get().error("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " produced no usable mip levels.");
                }
                else
                {
                    if (original_levels != 1 && mips.size() < target_levels)
                    {
                        Logger::get().error("[TextureManager] Injected DDS 0x" + format_hash_hex(hash) + " is missing mip levels (" + std::to_string(mips.size()) + "/" + std::to_string(target_levels) + "). Compressed replacements must ship a full mip chain; re-export with mipmaps to avoid shimmering at distance.");
                    }

                    // Use a concrete (non-TYPELESS) format so the SRV is valid.
                    DXGI_FORMAT view_fmt = dxgi_concrete_format(static_cast<DXGI_FORMAT>(dds.format));

                    D3D11_TEXTURE2D_DESC desc = {};
                    desc.Width = dds.width;
                    desc.Height = dds.height;
                    desc.MipLevels = static_cast<UINT>(mips.size());
                    desc.ArraySize = 1;
                    desc.Format = view_fmt;
                    desc.SampleDesc.Count = 1;
                    desc.SampleDesc.Quality = 0;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;

                    std::vector<D3D11_SUBRESOURCE_DATA> subres_data(mips.size());
                    for (size_t i = 0; i < mips.size(); ++i)
                    {
                        subres_data[i].pSysMem = mips[i].ptr;
                        subres_data[i].SysMemPitch = mips[i].row_pitch;
                        subres_data[i].SysMemSlicePitch = mips[i].slice_pitch;
                    }

                    ID3D11Texture2D *highres_tex = nullptr;
                    D3D11Hook::s_inside_injection = true;
                    HRESULT hr = device->CreateTexture2D(&desc, subres_data.data(), &highres_tex);
                    if (SUCCEEDED(hr) && highres_tex != nullptr)
                    {
                        ID3D11ShaderResourceView *highres_srv = nullptr;
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = desc.Format;
                        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srv_desc.Texture2D.MostDetailedMip = 0;
                        srv_desc.Texture2D.MipLevels = desc.MipLevels;

                        hr = device->CreateShaderResourceView(highres_tex, &srv_desc, &highres_srv);
                        highres_tex->Release(); // SRV holds the texture reference

                        if (SUCCEEDED(hr) && highres_srv != nullptr)
                        {
                            // Keep our reference (released in release_replacements).
                            m_d3d11_replacements[hash] = highres_srv;

                            details.status = TextureStatus::INJECTED;
                            details.filepath_injected = inject_path.string();
                            details.replacement_handle = reinterpret_cast<uint64_t>(highres_srv);
                            details.repl_width = dds.width;
                            details.repl_height = dds.height;

                            Logger::get().info("[TextureManager] Loaded DX11 replacement for 0x" + format_hash_hex(hash) + " (" + std::to_string(dds.width) + "x" + std::to_string(dds.height) + ", " + std::to_string(mips.size()) + " mips, original had " + std::to_string(original_levels) + ")");
                        }
                    }
                    else
                    {
                        Logger::get().error("[TextureManager] Failed to create DX11 replacement texture for 0x" + format_hash_hex(hash) + ", HRESULT: " + std::to_string(hr));
                    }
                    D3D11Hook::s_inside_injection = false;
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to load injected DDS file " + inject_path.string());
            }
        }

        m_tracked_textures[hash] = details;

        if (auto_dump)
        {
            dump_texture(hash, width, height, format, pixel_data, pitch);
            m_tracked_textures[hash].status = TextureStatus::DUMPED;
        }
    }

    std::vector<TextureDetails> TextureManager::get_active_textures()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<TextureDetails> result;
        result.reserve(m_tracked_textures.size());

        for (auto &pair : m_tracked_textures)
        {
            if (m_injected_files.find(pair.first) != m_injected_files.end() || pair.second.replacement_handle != 0)
            {
                pair.second.status = TextureStatus::INJECTED;
            }

            if (show_current_frame_only)
            {
                if (pair.second.last_seen_frame == 0 || (m_frame_count > 0 && pair.second.last_seen_frame + 60 < m_frame_count))
                    continue;
            }
            result.push_back(pair.second);
        }
        return result;
    }

    bool TextureManager::dump_texture(uint32_t hash, UINT width, UINT height, DXGI_FORMAT format, const void *data, UINT row_pitch)
    {
        reshade::api::format reshade_fmt = static_cast<reshade::api::format>(format);
        UINT slice_pitch = reshade::api::format_slice_pitch(reshade_fmt, row_pitch, height);
        if (slice_pitch == 0)
        {
            slice_pitch = row_pitch * height;
        }

        DumpRequest req;
        req.hash = hash;
        req.width = width;
        req.height = height;
        req.format = format;
        req.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + slice_pitch);
        req.row_pitch = row_pitch;

        {
            std::lock_guard<std::mutex> lock(m_dump_mutex);
            m_dump_queue.push_back(std::move(req));
        }
        m_dump_cv.notify_one();
        return true;
    }

    std::string TextureManager::write_dump_dds(uint32_t hash, UINT width, UINT height, DXGI_FORMAT format, const void *data, UINT row_pitch)
    {
        if (data == nullptr || width == 0 || height == 0)
            return {};

        // Write a concrete format, not TYPELESS, so the .dds is viewable and re-injectable.
        format = dxgi_concrete_format(format);

        std::error_code ec;
        std::filesystem::create_directories(m_dump_dir, ec);

        std::filesystem::path dds_path = m_dump_dir / (format_hash_hex(hash) + ".dds");

        reshade::api::format reshade_fmt = static_cast<reshade::api::format>(format);
        UINT slice_pitch = reshade::api::format_slice_pitch(reshade_fmt, row_pitch, height);
        if (slice_pitch == 0)
            slice_pitch = row_pitch * height;

        reshade::api::resource_desc desc;
        desc.texture.width = width;
        desc.texture.height = height;
        desc.texture.levels = 1;
        desc.texture.format = reshade_fmt;

        reshade::api::subresource_data subres;
        subres.data = const_cast<void *>(data); // save_dds only reads it
        subres.row_pitch = row_pitch;
        subres.slice_pitch = slice_pitch;

        if (!save_dds(dds_path.string(), desc, subres))
            return {};
        return dds_path.string();
    }

    void TextureManager::dump_worker_loop()
    {
        while (true)
        {
            DumpRequest req;
            {
                std::unique_lock<std::mutex> lock(m_dump_mutex);
                m_dump_cv.wait(lock, [this]() { return !m_dump_thread_running || !m_dump_queue.empty(); });

                if (!m_dump_thread_running && m_dump_queue.empty())
                    break;

                req = std::move(m_dump_queue.front());
                m_dump_queue.erase(m_dump_queue.begin());
            }

            std::string path = write_dump_dds(req.hash, req.width, req.height, req.format, req.data.data(), req.row_pitch);
            if (!path.empty())
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tracked_textures.find(req.hash);
                if (it != m_tracked_textures.end())
                {
                    it->second.status = TextureStatus::DUMPED;
                    it->second.filepath_dumped = path;
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to dump texture 0x" + format_hash_hex(req.hash));
            }
        }
    }

    std::string TextureManager::dump_resource11(uint32_t hash, ID3D11Resource *res)
    {
        ID3D11Device *device = D3D11Hook::get().get_device();
        ID3D11DeviceContext *ctx = D3D11Hook::get().get_context();
        if (device == nullptr || ctx == nullptr || res == nullptr)
            return {};

        std::string path;
        ID3D11Texture2D *tex2d = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex2d))) && tex2d != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            tex2d->GetDesc(&desc);

            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;

            // Keep the re-entrancy guard set across the whole readback. Our staging
            // CreateTexture2D/Map/Unmap all pass through our own hooks; without the guard
            // Hooked_Unmap would re-enter register_unmap_texture11, which locks m_mutex -
            // and request_dump already holds it on this thread, so that recursive lock is
            // undefined behaviour (observed as a crash right after the .dds was written).
            ID3D11Texture2D *staging_tex = nullptr;
            D3D11Hook::s_inside_injection = true;
            HRESULT hr = device->CreateTexture2D(&staging, nullptr, &staging_tex);

            if (SUCCEEDED(hr) && staging_tex != nullptr)
            {
                ctx->CopyResource(staging_tex, tex2d);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(ctx->Map(staging_tex, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData != nullptr)
                {
                    path = write_dump_dds(hash, desc.Width, desc.Height, desc.Format, mapped.pData, mapped.RowPitch);
                    ctx->Unmap(staging_tex, 0);
                }
                staging_tex->Release();
            }
            D3D11Hook::s_inside_injection = false;
            tex2d->Release();
        }
        return path;
    }

    std::string TextureManager::dump_base_texture9(uint32_t hash, IDirect3DBaseTexture9 *base)
    {
        if (base == nullptr)
            return {};

        IDirect3DTexture9 *tex = nullptr;
        if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&tex))) || tex == nullptr)
            return {};

        std::string path;
        D3DSURFACE_DESC sd = {};
        if (SUCCEEDED(tex->GetLevelDesc(0, &sd)))
        {
            DXGI_FORMAT dxgi = d3d9_format_to_dxgi(sd.Format);

            D3D9Hook::s_inside_injection = true;

            D3DLOCKED_RECT lr = {};
            if (dxgi != DXGI_FORMAT_UNKNOWN && SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                // Lockable pool (managed / system memory / dynamic).
                if (lr.pBits != nullptr)
                    path = write_dump_dds(hash, sd.Width, sd.Height, dxgi, lr.pBits, lr.Pitch);
                tex->UnlockRect(0);
            }
            else if (dxgi != DXGI_FORMAT_UNKNOWN && (sd.Usage & D3DUSAGE_RENDERTARGET))
            {
                // Render target: copy to a system-memory surface, then read that back.
                IDirect3DDevice9 *dev = D3D9Hook::get().get_device();
                IDirect3DSurface9 *src = nullptr;
                IDirect3DSurface9 *dst = nullptr;
                if (dev != nullptr && SUCCEEDED(tex->GetSurfaceLevel(0, &src)) && src != nullptr)
                {
                    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &dst, nullptr)) && dst != nullptr)
                    {
                        D3DLOCKED_RECT lr2 = {};
                        if (SUCCEEDED(dev->GetRenderTargetData(src, dst)) &&
                            SUCCEEDED(dst->LockRect(&lr2, nullptr, D3DLOCK_READONLY)))
                        {
                            path = write_dump_dds(hash, sd.Width, sd.Height, dxgi, lr2.pBits, lr2.Pitch);
                            dst->UnlockRect();
                        }
                        dst->Release();
                    }
                    src->Release();
                }
            }

            D3D9Hook::s_inside_injection = false;
        }

        tex->Release();
        return path; // request_dump reports the failure with context
    }

    bool TextureManager::request_dump(uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_tracked_textures.find(hash);
        if (it == m_tracked_textures.end())
            return false;
        TextureDetails &d = it->second;

        // We read back only the live handle pinned while the texture is on screen. Reading
        // an arbitrary tracked pointer is unsafe (it may have been freed and its address
        // reused), so dumping requires the texture to be visible when the button is clicked.
        std::string path;
        if (hash != m_preview_target_hash)
        {
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": select the texture first.");
            return false;
        }

        bool attempted = false;
        if (d.is_dx11 && m_preview_srv11 != nullptr)
        {
            attempted = true;
            ID3D11Resource *res = nullptr;
            m_preview_srv11->GetResource(&res);
            if (res != nullptr)
            {
                path = dump_resource11(hash, res);
                res->Release();
            }
        }
        else if (!d.is_dx11 && m_preview_tex9 != nullptr)
        {
            attempted = true;
            path = dump_base_texture9(hash, m_preview_tex9);
        }

        if (!path.empty())
        {
            d.status = TextureStatus::DUMPED;
            d.filepath_dumped = path;
            Logger::get().info("[TextureManager] Dumped 0x" + format_hash_hex(hash) + " to " + path);
            return true;
        }

        if (!attempted)
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": it is not currently on screen. Select it while it is being drawn, then Dump.");
        else
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": this texture cannot be read back on demand (D3D9 default-pool). Turn on Auto-dump to capture it from the upload at load time.");
        return false;
    }

    size_t TextureManager::dump_all(bool scene_only)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t queued = 0;
        for (auto &pair : m_tracked_textures)
        {
            if (scene_only)
            {
                const TextureDetails &d = pair.second;
                bool active = d.last_seen_frame != 0 && !(m_frame_count > 0 && d.last_seen_frame + 60 < m_frame_count);
                if (!active)
                    continue;
            }
            m_pending_dumps.insert(pair.first);
            ++queued;
        }
        Logger::get().info("[TextureManager] Dump-all queued " + std::to_string(queued) + (scene_only ? " active" : " tracked") + " texture(s); each is written the next time it is drawn.");
        return queued;
    }

    // Drains a few queued bulk-dump readbacks per frame. Caller MUST hold m_mutex.
    void TextureManager::process_readback_queue()
    {
        int budget = 8;
        while (!m_readback_queue.empty() && budget-- > 0)
        {
            PendingReadback rb = m_readback_queue.back();
            m_readback_queue.pop_back();

            std::string path;
            if (rb.srv11 != nullptr)
            {
                ID3D11Resource *res = nullptr;
                rb.srv11->GetResource(&res);
                if (res != nullptr)
                {
                    path = dump_resource11(rb.hash, res);
                    res->Release();
                }
                rb.srv11->Release();
            }
            else if (rb.tex9 != nullptr)
            {
                path = dump_base_texture9(rb.hash, rb.tex9);
                rb.tex9->Release();
            }

            if (!path.empty())
            {
                auto it = m_tracked_textures.find(rb.hash);
                if (it != m_tracked_textures.end())
                {
                    it->second.status = TextureStatus::DUMPED;
                    it->second.filepath_dumped = path;
                }
            }
        }
    }
}
