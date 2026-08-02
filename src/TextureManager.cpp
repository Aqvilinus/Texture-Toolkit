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
        m_dump_dir = m_game_dir / "TT" / "dump";
        m_inject_dir = m_game_dir / "TT" / "inject";

        // Load settings from config
        const auto& cfg = ConfigManager::get().get_config();
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
        uint64_t handle = reinterpret_cast<uint64_t>(texture);

        // Tag the original resource with its content hash. Used both for active-scene
        // tracking and for resolving replacements at bind time (SetTexture), and is
        // immune to driver pointer reuse.
        texture->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.hash_hex_0x = format_hash_hex_0x(hash);
        details.width = width;
        details.height = height;
        details.mip_levels = original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "D3DFORMAT_" + d3d9_format_to_string(format);
        details.format_short = "D3D9_" + d3d9_format_to_string(format);
        details.resource_handle = handle;
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
                                details.width = dds.width;
                                details.height = dds.height;

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

        bool should_dump = auto_dump || (m_requested_dumps.find(hash) != m_requested_dumps.end());
        if (should_dump)
        {
            DXGI_FORMAT dxgi_fmt = d3d9_format_to_dxgi(format);
            if (dxgi_fmt != DXGI_FORMAT_UNKNOWN)
            {
                dump_texture(hash, width, height, dxgi_fmt, pixel_data, pitch);
                m_tracked_textures[hash].status = TextureStatus::DUMPED;
                m_requested_dumps.erase(hash);
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

        // Original mip count drives the replacement's mip topology.
        UINT original_levels = 1;
        {
            ID3D11Texture2D *orig_tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&orig_tex))) && orig_tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC od = {};
                orig_tex->GetDesc(&od);
                original_levels = od.MipLevels; // 0 = full chain generated by the runtime
                orig_tex->Release();
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        uint64_t handle = reinterpret_cast<uint64_t>(resource);

        // Tag the original resource with its content hash (see get_replacement_srv11).
        resource->SetPrivateData(TT_HASH_GUID, sizeof(hash), &hash);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.hash_hex_0x = format_hash_hex_0x(hash);
        details.width = width;
        details.height = height;
        details.mip_levels = (original_levels == 0) ? full_mip_count(width, height) : original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "DXGI_FORMAT_" + std::to_string(static_cast<uint32_t>(format));
        details.format_short = "DX11_" + std::to_string(static_cast<uint32_t>(format));
        details.resource_handle = handle;
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

                    D3D11_TEXTURE2D_DESC desc = {};
                    desc.Width = dds.width;
                    desc.Height = dds.height;
                    desc.MipLevels = static_cast<UINT>(mips.size());
                    desc.ArraySize = 1;
                    desc.Format = static_cast<DXGI_FORMAT>(dds.format);
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
                            details.width = dds.width;
                            details.height = dds.height;

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

        bool should_dump = auto_dump || (m_requested_dumps.find(hash) != m_requested_dumps.end());
        if (should_dump)
        {
            dump_texture(hash, width, height, format, pixel_data, pitch);
            m_tracked_textures[hash].status = TextureStatus::DUMPED;
            m_requested_dumps.erase(hash);
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

            std::error_code ec;
            std::filesystem::create_directories(m_dump_dir, ec);

            std::string hex_str = format_hash_hex(req.hash);
            std::filesystem::path dds_path = m_dump_dir / (hex_str + ".dds");

            reshade::api::resource_desc desc;
            desc.texture.width = req.width;
            desc.texture.height = req.height;
            desc.texture.levels = 1;
            desc.texture.format = static_cast<reshade::api::format>(req.format);

            reshade::api::subresource_data subres;
            subres.data = req.data.data();
            subres.row_pitch = req.row_pitch;
            subres.slice_pitch = static_cast<UINT>(req.data.size());

            if (save_dds(dds_path.string(), desc, subres))
            {
                Logger::get().debug("[TextureManager] Dumped texture hash 0x" + hex_str + " (" + std::to_string(req.width) + "x" + std::to_string(req.height) + ") asynchronously to TT/dump");
                
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_tracked_textures.find(req.hash) != m_tracked_textures.end())
                {
                    m_tracked_textures[req.hash].status = TextureStatus::DUMPED;
                    m_tracked_textures[req.hash].filepath_dumped = dds_path.string();
                }
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to dump texture hash 0x" + hex_str + " asynchronously");
            }
        }
    }

    void TextureManager::request_dump(uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requested_dumps.insert(hash);

        auto it = m_tracked_textures.find(hash);
        if (it == m_tracked_textures.end() || it->second.resource_handle == 0)
            return;

        // Instant DX11 Staging Texture Readback
        ID3D11Device *device11 = D3D11Hook::get().get_device();
        ID3D11DeviceContext *context11 = D3D11Hook::get().get_context();
        if (device11 != nullptr && context11 != nullptr)
        {
            ID3D11Resource *res = reinterpret_cast<ID3D11Resource *>(it->second.resource_handle);
            ID3D11Texture2D *tex2d = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex2d))) && tex2d != nullptr)
            {
                D3D11_TEXTURE2D_DESC desc = {};
                tex2d->GetDesc(&desc);

                D3D11_TEXTURE2D_DESC staging_desc = desc;
                staging_desc.Usage = D3D11_USAGE_STAGING;
                staging_desc.BindFlags = 0;
                staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                staging_desc.MiscFlags = 0;

                ID3D11Texture2D *staging_tex = nullptr;
                if (SUCCEEDED(device11->CreateTexture2D(&staging_desc, nullptr, &staging_tex)) && staging_tex != nullptr)
                {
                    context11->CopyResource(staging_tex, tex2d);

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(context11->Map(staging_tex, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData != nullptr)
                    {
                        dump_texture(hash, desc.Width, desc.Height, desc.Format, mapped.pData, mapped.RowPitch);
                        it->second.status = TextureStatus::DUMPED;
                        m_requested_dumps.erase(hash);
                        context11->Unmap(staging_tex, 0);
                        Logger::get().info("[TextureManager] Instant DX11 VRAM staging dump succeeded for 0x" + format_hash_hex(hash));
                    }
                    staging_tex->Release();
                }
                tex2d->Release();
            }
        }
    }
}
