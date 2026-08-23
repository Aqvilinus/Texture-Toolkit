#include "render/d3d9/d3d9_texture_manager.h"
#include "render/d3d9/d3d9_format.h"
#include "render/dxgi/dxgi_format.h"
#include "render/render_backend.h"
#include "texture/texture_hash.h"
#include "core/logger.h"
#include "core/scoped_flag.h"
#include "render/d3d9/d3d9_hook.h"

#include <DirectXTex.h>
#include <algorithm>
#include <vector>
#include <fstream>
#include <bit>
#include <cstring>

namespace TextureToolkit
{
    // One mip level into a locked rectangle. RGBA->BGRA is "keep green and alpha, swap red and
    // blue", which over a whole pixel is a 16-bit rotate of the two odd bytes: one load, three ALU
    // ops and one store instead of eight byte accesses, and it vectorises.
    static void upload_level(const D3DLOCKED_RECT &rect, const DirectX::Image &level, bool swizzle)
    {
        if (rect.pBits == nullptr || rect.Pitch <= 0)
            return;

        uint8_t *dest = static_cast<uint8_t *>(rect.pBits);
        const UINT dest_pitch = static_cast<UINT>(rect.Pitch);
        const UINT src_pitch = static_cast<UINT>(level.rowPitch);
        const UINT copy_pitch = std::min(dest_pitch, src_pitch);
        const UINT rows = static_cast<UINT>(DirectX::ComputeScanlines(level.format, level.height));

        if (swizzle)
        {
            for (UINT y = 0; y < rows; ++y)
            {
                const uint8_t *src_row = level.pixels + y * src_pitch;
                uint8_t *dest_row = dest + y * dest_pitch;
                for (UINT x = 0; x < level.width; ++x)
                {
                    uint32_t pixel = 0;
                    std::memcpy(&pixel, src_row + x * 4, 4);
                    pixel = (pixel & 0xFF00FF00u) | std::rotl(pixel & 0x00FF00FFu, 16);
                    std::memcpy(dest_row + x * 4, &pixel, 4);
                }
            }
        }
        else if (dest_pitch == src_pitch)
        {
            // The common case for block-compressed data straight out of a DDS: one copy per level.
            std::memcpy(dest, level.pixels, static_cast<size_t>(copy_pitch) * rows);
        }
        else
        {
            for (UINT y = 0; y < rows; ++y)
                std::memcpy(dest + y * dest_pitch, level.pixels + y * src_pitch, copy_pitch);
        }
    }

    uint32_t D3D9TextureManager::register_texture9(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch, bool auto_dump_here)
    {
        if (device == nullptr || texture == nullptr || pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        if (filter_small_textures && (width < 16 || height < 16))
            return 0;

        const uint32_t hash = hash_pixels(pixel_data, width, height, format, pitch);
        if (hash == 0)
        {
            // Reported from here rather than from the measuring code, which runs inside the game's
            // unlock hook before any lock of ours is held. Under m_mutex below it would be a
            // container written from two threads at once on a game that uploads from a worker.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_unmeasurable_formats.insert(static_cast<uint32_t>(format)).second)
            {
                Logger::get().warn("[D3D9Textures] Not tracking textures in " + format_name(format) +
                                   ": no known pixel size, and guessing one would read past the lock.");
            }
            return 0;
        }

        UINT original_levels = texture->GetLevelCount();

        std::lock_guard<std::mutex> lock(m_mutex);

        // Tag the original resource with its content hash. Used both for active-scene
        // tracking and for resolving replacements at bind time (SetTexture), and is
        // immune to driver pointer reuse.
        texture->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);

        const bool dx9_compressed = is_block_compressed(format);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.width = width;
        details.height = height;
        details.mip_levels = original_levels;
        details.format_id = static_cast<uint32_t>(format);
        // format_name prefixes only the numeric fallback, so a known format arrives bare.
        const std::string name = format_name(format);
        details.format_str = name;
        details.format_short = "D3D9_" + name;
        details.is_compressed = dx9_compressed;
        details.is_srgb = false; // D3D9 sRGB is a sampler state, not part of the format
        details.is_dx11 = false;
        {
            DXGI_FORMAT ddx = to_dxgi(format);
            details.data_size = (ddx != DXGI_FORMAT_UNKNOWN)
                ? texture_byte_size(ddx, width, height, original_levels)
                : width * height * 4;
        }
        details.last_seen_ticks = now_ticks();

        std::filesystem::path inject_path = find_injection_path_locked(hash);
        if (enable_injection && !inject_path.empty() &&
            m_d3d9.replacements.find(hash) == m_d3d9.replacements.end())
        {
            build_replacement9(device, hash, inject_path, original_levels, details);
        }

        track(hash, details);
        if (Logger::get().debug_enabled())
            Logger::get().debug("[D3D9Textures] Tracked D3D9 texture: 0x" + details.hash_hex + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

        if (auto_dump && auto_dump_here)
        {
            DXGI_FORMAT dxgi_fmt = to_dxgi(format);
            if (dxgi_fmt != DXGI_FORMAT_UNKNOWN)
            {
                dump_texture(hash, width, height, dxgi_fmt, {copy_level(dxgi_fmt, height, pixel_data, pitch)});
            }
        }

        return hash;
    }

    static bool save_via_d3dx(IDirect3DBaseTexture9 *texture, const std::filesystem::path &path);

    void D3D9TextureManager::register_from_d3dx(IDirect3DDevice9 *device, IDirect3DTexture9 *texture)
    {
        if (device == nullptr || texture == nullptr)
            return;

        D3DSURFACE_DESC desc = {};
        if (FAILED(texture->GetLevelDesc(0, &desc)))
            return;

        uint32_t hash = 0;
        {
            // Our own lock, so the lock hook must not read it back as a game upload.
            ScopedFlag injecting(D3D9Hook::s_inside_injection);

            D3DLOCKED_RECT rect = {};
            if (FAILED(texture->LockRect(0, &rect, nullptr, D3DLOCK_READONLY)))
                return;

            if (rect.pBits != nullptr && rect.Pitch > 0)
            {
                // Registered without its usual auto-dump: D3DX can write the whole mip chain from
                // the texture itself, which is the point of watching this entry at all.
                hash = register_texture9(device, texture, rect.pBits, desc.Width, desc.Height, desc.Format,
                                         static_cast<UINT>(rect.Pitch), /*auto_dump_here=*/false);
            }
            texture->UnlockRect(0);
        }

        if (hash == 0 || !auto_dump)
            return;

        const std::filesystem::path dds_path = dump_path_for(hash);
        if (save_via_d3dx(texture, dds_path))
            note_dumped(hash, dds_path.string());
    }

    // original_levels is the original texture's level count. Caller MUST hold m_mutex.
    // D3DX can build a texture our own path refuses: it converts a format the device does not
    // support, rescales past the device's limit and pads to a power of two. We never load it --
    // GetModuleHandle only, exactly as Special K does and like them only the last version --
    // so a game that ships without it is
    // unaffected, and a game that uses it lends us the copy it already has.
    // Never loaded by us, only borrowed from a game that already has it, and only the last version
    // -- exactly as Special K does. A game without D3DX gets a null and the paths below fall back.
    template <typename Fn>
    static Fn d3dx9_proc(const char *name)
    {
        HMODULE module = GetModuleHandleW(L"d3dx9_43.dll");
        return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
    }

    static IDirect3DTexture9 *create_via_d3dx(IDirect3DDevice9 *device, const std::filesystem::path &path)
    {
        using CreateFromMemory_fn = HRESULT(WINAPI *)(IDirect3DDevice9 *, LPCVOID, UINT, UINT, UINT, UINT,
                                                      DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR,
                                                      void *, void *, IDirect3DTexture9 **);
        static CreateFromMemory_fn create = d3dx9_proc<CreateFromMemory_fn>("D3DXCreateTextureFromFileInMemoryEx");

        if (create == nullptr || device == nullptr)
            return nullptr;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return nullptr;

        const std::streamsize size = file.tellg();
        if (size <= 0)
            return nullptr;

        file.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
            return nullptr;

        constexpr UINT kDefault = static_cast<UINT>(-1);   // D3DX_DEFAULT: let it choose

        IDirect3DTexture9 *texture = nullptr;
        ScopedFlag injecting(D3D9Hook::s_inside_injection);
        const HRESULT hr = create(device, bytes.data(), static_cast<UINT>(bytes.size()),
                                  kDefault, kDefault, kDefault, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
                                  kDefault, kDefault, 0, nullptr, nullptr, &texture);
        return SUCCEEDED(hr) ? texture : nullptr;
    }

    // Special K dumps by handing D3DX the texture object rather than a buffer of pixels, which
    // writes every level it has -- and every format, including the ones with no DXGI equivalent
    // that our own writer has to refuse. Resolved the same way as the loader above: never loaded
    // by us, only borrowed from a game that already has it.
    static bool save_via_d3dx(IDirect3DBaseTexture9 *texture, const std::filesystem::path &path)
    {
        using SaveTextureToFileW_fn = HRESULT(WINAPI *)(LPCWSTR, DWORD, IDirect3DBaseTexture9 *, const PALETTEENTRY *);
        static SaveTextureToFileW_fn save = d3dx9_proc<SaveTextureToFileW_fn>("D3DXSaveTextureToFileW");

        if (save == nullptr || texture == nullptr)
            return false;

        constexpr DWORD kDDS = 4;   // D3DXIFF_DDS

        ScopedFlag injecting(D3D9Hook::s_inside_injection);
        return SUCCEEDED(save(path.wstring().c_str(), kDDS, texture, nullptr));
    }

    // A render target cannot be read by D3DX where it lives, so it is copied level by level into a
    // texture that can be, exactly as Special K does.
    static IDirect3DTexture9 *copy_render_target(IDirect3DTexture9 *texture, const D3DSURFACE_DESC &desc)
    {
        IDirect3DDevice9 *device = D3D9Hook::get().get_device();
        if (device == nullptr)
            return nullptr;

        ScopedFlag injecting(D3D9Hook::s_inside_injection);

        const DWORD levels = texture->GetLevelCount();

        IDirect3DTexture9 *copy = nullptr;
        if (FAILED(device->CreateTexture(desc.Width, desc.Height, levels,
                                         D3DUSAGE_DYNAMIC, desc.Format, D3DPOOL_DEFAULT, &copy, nullptr)) ||
            copy == nullptr)
            return nullptr;

        for (DWORD level = 0; level < levels; ++level)
        {
            IDirect3DSurface9 *from = nullptr;
            IDirect3DSurface9 *to = nullptr;
            if (SUCCEEDED(texture->GetSurfaceLevel(level, &from)) &&
                SUCCEEDED(copy->GetSurfaceLevel(level, &to)))
            {
                device->GetRenderTargetData(from, to);
            }
            if (from != nullptr) from->Release();
            if (to != nullptr) to->Release();
        }

        return copy;
    }

    bool D3D9TextureManager::build_replacement9(IDirect3DDevice9 *device, uint32_t hash, const std::filesystem::path &inject_path, UINT original_levels, TextureDetails &details)
    {
        if (device == nullptr)
            return false;

        bool created = false;
        {
            DirectX::ScratchImage image;
            DirectX::TexMetadata meta = {};
            if (load_dds(inject_path, original_levels > 1, image, meta))
            {
                D3DFORMAT d3d9_target_fmt = to_d3d9(meta.format);
                if (d3d9_target_fmt == D3DFMT_UNKNOWN)
                {
                    if (IDirect3DTexture9 *via_d3dx = create_via_d3dx(device, inject_path))
                    {
                        m_d3d9.replacements[hash].Attach(via_d3dx);
                        details.status = TextureStatus::INJECTED;
                        details.filepath_injected = inject_path.string();
                        details.replacement_handle = reinterpret_cast<uint64_t>(via_d3dx);

                        // From the texture, not the file: D3DX converts the format and may rescale
                        // or pad to what the device accepts, so the file's size is not what exists.
                        D3DSURFACE_DESC built = {};
                        if (SUCCEEDED(via_d3dx->GetLevelDesc(0, &built)))
                        {
                            details.repl_width = built.Width;
                            details.repl_height = built.Height;
                        }

                        Logger::get().info("[D3D9Textures] Replacement for 0x" + format_hash_hex(hash) +
                                           " built by the game's own D3DX: " +
                                           std::to_string(details.repl_width) + "x" + std::to_string(details.repl_height) +
                                           " (no DX9 format for the file's " + format_name(meta.format) + ").");
                        return true;
                    }

                    Logger::get().error("[D3D9Textures] No DX9 format for injected texture " +
                                        inject_path.string() + ", and the game has no D3DX to fall back on.");
                }
                else
                {
                    // Match the original's mip topology: single level stays single,
                    // a mipmapped original gets a full chain (auto-generated as needed).
                    const uint32_t target_levels = (original_levels <= 1)
                                                       ? 1u
                                                       : full_mip_count(static_cast<uint32_t>(meta.width), static_cast<uint32_t>(meta.height));

                    if (meta.mipLevels == 0)
                    {
                        Logger::get().error("[D3D9Textures] Injected DDS 0x" + format_hash_hex(hash) + " produced no usable mip levels.");
                    }
                    else
                    {
                        if (original_levels > 1 && meta.mipLevels < target_levels)
                        {
                            Logger::get().error("[D3D9Textures] Injected DDS 0x" + format_hash_hex(hash) + " is missing mip levels (" + std::to_string(meta.mipLevels) + "/" + std::to_string(target_levels) + "). Compressed replacements must ship a full mip chain; re-export with mipmaps to avoid shimmering at distance.");
                        }

                        IDirect3DTexture9 *highres_tex = nullptr;
                        ScopedFlag injecting(D3D9Hook::s_inside_injection);
                        HRESULT hr = device->CreateTexture(
                            static_cast<UINT>(meta.width), static_cast<UINT>(meta.height), static_cast<UINT>(meta.mipLevels), 0,
                            d3d9_target_fmt, D3DPOOL_MANAGED, &highres_tex, nullptr);

                        bool needs_swizzle = (meta.format == DXGI_FORMAT_R8G8B8A8_UNORM || meta.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

                        if (SUCCEEDED(hr) && highres_tex != nullptr)
                        {
                            bool upload_ok = true;
                            for (size_t lvl = 0; lvl < meta.mipLevels; ++lvl)
                            {
                                D3DLOCKED_RECT rect = {};
                                if (FAILED(highres_tex->LockRect(static_cast<UINT>(lvl), &rect, nullptr, 0)))
                                {
                                    upload_ok = false;
                                    break;
                                }

                                const DirectX::Image *m = image.GetImage(lvl, 0, 0);
                                if (m == nullptr)
                                {
                                    upload_ok = false;
                                    break;
                                }
                                upload_level(rect, *m, needs_swizzle);

                                highres_tex->UnlockRect(static_cast<UINT>(lvl));
                            }

                            if (upload_ok)
                            {
                                // Keep our reference (released in release_replacements).
                                m_d3d9.replacements[hash].Attach(highres_tex);

                                details.status = TextureStatus::INJECTED;
                                details.filepath_injected = inject_path.string();
                                details.replacement_handle = reinterpret_cast<uint64_t>(highres_tex);
                                details.repl_width = static_cast<uint32_t>(meta.width);
                                details.repl_height = static_cast<uint32_t>(meta.height);
                                created = true;

                                Logger::get().info("[D3D9Textures] Loaded high-res DX9 replacement for 0x" + format_hash_hex(hash) +
                                                   " (" + std::to_string(details.repl_width) + "x" + std::to_string(details.repl_height) +
                                                   ", " + std::to_string(image.GetImageCount()) + " level(s), original had " +
                                                   std::to_string(original_levels) + ")");
                            }
                            else
                            {
                                highres_tex->Release();
                                Logger::get().error("[D3D9Textures] Failed to upload mip data for DX9 replacement 0x" + format_hash_hex(hash));
                            }
                        }
                        else
                        {
                            Logger::get().error("[D3D9Textures] Failed to create high-res replacement D3D9 texture for 0x" + format_hash_hex(hash));
                        }
                    }
                }
            }
            else
            {
                Logger::get().error("[D3D9Textures] Failed to load injected DDS file " + inject_path.string());
            }
        }
        return created;
    }

    IDirect3DBaseTexture9 *D3D9TextureManager::get_replacement_texture9(IDirect3DBaseTexture9 *orig)
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

        if (hash == m_preview_target_hash && m_d3d9.preview == nullptr)
        {
            orig->AddRef();
            m_d3d9.preview = orig;
        }

        if (has_pending_dumps() && take_pending_dump(hash))
        {
            orig->AddRef();
            queue_readback(hash, orig, nullptr);
        }

        if (!enable_injection)
            return orig;

        auto it = m_d3d9.replacements.find(hash);
        if (it == m_d3d9.replacements.end())
        {
            // Hot reload: a DDS added after this texture was uploaded has no replacement yet, and
            // the game will not upload it again. Only flag it here; the build happens in on_frame,
            // never inside this draw call (see process_pending_injections).
            note_pending_injection(hash);
        }
        if (it != m_d3d9.replacements.end() && it->second != nullptr)
            return it->second.Get();

        return orig;
    }

    void D3D9TextureManager::copy_tag9(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
    {
        if (src == nullptr || dst == nullptr)
            return;

        uint32_t hash = 0;
        DWORD size = sizeof(hash);
        if (SUCCEEDED(src->GetPrivateData(TT_HASH_GUID, &hash, &size)) && size == sizeof(hash))
            dst->SetPrivateData(TT_HASH_GUID, &hash, sizeof(hash), 0);
    }

    std::string D3D9TextureManager::dump_base_texture9(uint32_t hash, IDirect3DBaseTexture9 *base)
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
            // D3DX first, the way Special K does it: it writes the whole texture, so every level
            // and every format survive. Our own writer below is what a game without D3DX gets.
            {
                const std::filesystem::path dds_path = dump_path_for(hash);

                if ((sd.Usage & D3DUSAGE_RENDERTARGET) != 0)
                {
                    if (IDirect3DTexture9 *copy = copy_render_target(tex, sd))
                    {
                        const bool saved = save_via_d3dx(copy, dds_path);
                        copy->Release();
                        if (saved)
                        {
                            tex->Release();
                            return dds_path.string();
                        }
                    }
                }
                else if (save_via_d3dx(tex, dds_path))
                {
                    tex->Release();
                    return dds_path.string();
                }
            }

            DXGI_FORMAT dxgi = to_dxgi(sd.Format);

            ScopedFlag injecting(D3D9Hook::s_inside_injection);

            D3DLOCKED_RECT lr = {};
            if (dxgi != DXGI_FORMAT_UNKNOWN && SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)))
            {
                if (lr.pBits != nullptr && lr.Pitch > 0)
                {
                    // Every level, unlike the auto-dump path: there the game delivers one level per
                    // LockRect and we never learn when the chain is complete, but a texture already
                    // on the device can simply be walked.
                    DumpLevels levels;
                    levels.push_back(copy_level(dxgi, sd.Height, lr.pBits, lr.Pitch));

                    const DWORD level_count = tex->GetLevelCount();
                    for (DWORD level = 1; level < level_count; ++level)
                    {
                        D3DSURFACE_DESC ld = {};
                        D3DLOCKED_RECT lrn = {};
                        if (FAILED(tex->GetLevelDesc(level, &ld)) ||
                            FAILED(tex->LockRect(level, &lrn, nullptr, D3DLOCK_READONLY)))
                            break;

                        if (lrn.pBits != nullptr && lrn.Pitch > 0)
                            levels.push_back(copy_level(dxgi, ld.Height, lrn.pBits, lrn.Pitch));
                        tex->UnlockRect(level);

                        // A short chain is written as far as it goes rather than not at all.
                        if (levels.size() != level + 1)
                            break;
                    }

                    path = write_dump_dds(hash, sd.Width, sd.Height, dxgi, std::move(levels));
                }
                tex->UnlockRect(0);
            }
            else if (dxgi != DXGI_FORMAT_UNKNOWN && (sd.Usage & D3DUSAGE_RENDERTARGET))
            {
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
                            if (lr2.pBits == nullptr || lr2.Pitch <= 0)
                            {
                                dst->UnlockRect();
                                dst->Release();
                                src->Release();
                                tex->Release();
                                return {};
                            }
                            path = write_dump_dds(hash, sd.Width, sd.Height, dxgi, {copy_level(dxgi, sd.Height, lr2.pBits, lr2.Pitch)});
                            dst->UnlockRect();
                        }
                        dst->Release();
                    }
                    src->Release();
                }
            }

        }

        tex->Release();
        return path; // request_dump reports the failure with context
    }

    // Flags a drawn texture that has an inject file but no live replacement yet. Cheap: this runs
    // inside the game's draw call, so it only records the hash. Caller MUST hold m_mutex.
    // Caller MUST hold m_mutex.
    void D3D9TextureManager::process_pending_injections()
    {
        if (m_pending_injections.empty())
            return;

        // One per frame: a build reads the DDS off disk and creates the texture synchronously,
        // inside Present. Two of those in a frame is a visible hitch while an area streams in.
        int budget = 1;
        while (!m_pending_injections.empty() && budget-- > 0)
        {
            const uint32_t hash = *m_pending_injections.begin();
            m_pending_injections.erase(m_pending_injections.begin());

            auto fit = m_injected_files.find(hash);
            auto tit = m_tracked_textures.find(hash);
            if (fit == m_injected_files.end() || tit == m_tracked_textures.end())
                continue;

            if (m_d3d9.replacements.find(hash) != m_d3d9.replacements.end())
                continue;

            // No device yet is a "not now", not a "never": leave the flag off and let the next
            // draw re-raise it, instead of blacklisting a file that was never actually tried.
            IDirect3DDevice9 *dev9 = RenderBackend::get().d3d9_device();
            if (dev9 == nullptr)
                continue;

            TextureDetails &details = tit->second;
            const bool ok = build_replacement9(dev9, hash, fit->second, details.mip_levels, details);

            if (!ok)
                m_failed_injections.insert(hash); // do not retry a broken file every frame
        }
    }

    size_t D3D9TextureManager::refresh_branch()
    {
        // Draw-time substitution: dropping the replacements is enough, the next draw rebuilds them.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_d3d9.replacements.clear();

        size_t dropped = 0;
        for (auto &pair : m_tracked_textures)
        {
            if (pair.second.is_dx11)
                continue;

            pair.second.replacement_handle = 0;
            pair.second.repl_width = 0;
            pair.second.repl_height = 0;
            pair.second.filepath_injected.clear();
            if (pair.second.status == TextureStatus::INJECTED)
                pair.second.status = TextureStatus::ORIGINAL;
            ++dropped;
        }
        return dropped;
    }

    void D3D9TextureManager::process_branch_injections()
    {
        process_pending_injections();
    }

    std::string D3D9TextureManager::dump_selected(uint32_t hash)
    {
        return (m_d3d9.preview != nullptr) ? dump_base_texture9(hash, m_d3d9.preview.Get()) : std::string();
    }

    std::string D3D9TextureManager::dump_readback(const PendingReadback &rb)
    {
        return (rb.tex9 != nullptr) ? dump_base_texture9(rb.hash, rb.tex9) : std::string();
    }

    uint64_t D3D9TextureManager::branch_preview_handle() const
    {
        return reinterpret_cast<uint64_t>(m_d3d9.preview.Get());
    }

    uint64_t D3D9TextureManager::branch_file_preview_handle() const
    {
        return reinterpret_cast<uint64_t>(m_d3d9.file_preview.Get());
    }

    void D3D9TextureManager::release_branch_replacements()
    {
        m_d3d9.replacements.clear();
    }

    void D3D9TextureManager::release_branch_file_preview()
    {
        m_d3d9.file_preview.Reset();
    }

    void D3D9TextureManager::release_branch_preview()
    {
        m_d3d9.preview.Reset();
        m_d3d9.file_preview.Reset();
    }

    D3D9TextureManager &D3D9TextureManager::get()
    {
        static D3D9TextureManager instance;
        return instance;
    }

    uint64_t D3D9TextureManager::upload_file_preview(const DirectX::Image &image)
    {
        IDirect3DDevice9 *dev = RenderBackend::get().d3d9_device();
        const D3DFORMAT fmt = to_d3d9(image.format);
        if (dev == nullptr || image.pixels == nullptr || fmt == D3DFMT_UNKNOWN)
            return 0;

        IDirect3DTexture9 *tex = nullptr;
        ScopedFlag injecting(D3D9Hook::s_inside_injection);
        if (SUCCEEDED(dev->CreateTexture(static_cast<UINT>(image.width), static_cast<UINT>(image.height), 1, 0,
                                         fmt, D3DPOOL_MANAGED, &tex, nullptr)) && tex != nullptr)
        {
            D3DLOCKED_RECT rect = {};
            if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
            {
                const bool needs_swizzle = (image.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                            image.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
                upload_level(rect, image, needs_swizzle);

                tex->UnlockRect(0);
                m_d3d9.file_preview.Attach(tex); // takes the reference CreateTexture gave us
            }
            else
            {
                tex->Release();
            }
        }

        return reinterpret_cast<uint64_t>(m_d3d9.file_preview.Get());
    }
}
