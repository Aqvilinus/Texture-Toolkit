#include "render/d3d11/d3d11_texture_manager.h"
#include "render/d3d11/d3d11_hook.h"
#include "render/dxgi/dxgi_format.h"
#include "render/render_backend.h"
#include "texture/texture_hash.h"
#include "core/logger.h"
#include "core/scoped_flag.h"

#include <DirectXTex.h>
#include <algorithm>

namespace TextureToolkit
{
    // Rows only, never the padding between them: that is the uploader's to choose, is undefined
    // where a driver picked it, and is what Special K leaves out of the value it names packs with.
    // Timed so the diagnostics report can say what this costs inside the game's own CreateTexture2D.
    static uint32_t hash_and_account(TextureManagerBase &manager, const void *pixels, size_t row_pitch,
                                     size_t row_bytes, size_t rows)
    {
#if TT_DIAGNOSTICS
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
#endif
        const uint32_t hash = compute_crc32c_rows(static_cast<const uint8_t *>(pixels), row_pitch, row_bytes, rows);
#if TT_DIAGNOSTICS
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        manager.stat_hash_ticks.fetch_add(static_cast<uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
        manager.stat_hash_bytes.fetch_add(row_bytes * rows, std::memory_order_relaxed);
#else
        (void)manager;
#endif
        return hash;
    }
    D3D11TextureManager &D3D11TextureManager::get()
    {
        static D3D11TextureManager instance;
        return instance;
    }

    size_t D3D11TextureManager::probe_slot(const D3D11State::OwnedSet &owned, void *key)
    {
        size_t i = (reinterpret_cast<uintptr_t>(key) >> 4) & owned.mask;
        while (true)
        {
            void *slot = owned.slots[i].load(std::memory_order_acquire);
            if (slot == nullptr || slot == key)
                return i;
            i = (i + 1) & owned.mask;
        }
    }

    void D3D11TextureManager::grow_locked(size_t capacity, bool carry_over)
    {
        auto owned = std::make_unique<D3D11State::OwnedSet>();
        owned->slots = std::make_unique<std::atomic<void *>[]>(capacity);
        owned->hashes = std::make_unique<uint32_t[]>(capacity);
        owned->last_used = std::make_unique<std::atomic<uint64_t>[]>(capacity);
        owned->overrides = std::make_unique<std::atomic<void *>[]>(capacity);
        owned->mask = capacity - 1;

        if (const D3D11State::OwnedSet *previous = m_d3d11.snapshot.load(std::memory_order_acquire))
        {
            for (size_t i = 0; carry_over && i <= previous->mask; ++i)
            {
                void *key = previous->slots[i].load(std::memory_order_relaxed);
                if (key == nullptr)
                    continue;

                size_t j = (reinterpret_cast<uintptr_t>(key) >> 4) & owned->mask;
                while (owned->slots[j].load(std::memory_order_relaxed) != nullptr)
                    j = (j + 1) & owned->mask;

                owned->hashes[j] = previous->hashes[i];
                owned->last_used[j].store(previous->last_used[i].load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                owned->overrides[j].store(previous->overrides[i].load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                owned->slots[j].store(key, std::memory_order_relaxed);
                owned->count++;
            }

        }

        // The replaced table is kept, not freed: a reader may still be walking it on the render
        // thread and there is no handshake to prove otherwise. Growth doubles, so a session ends up
        // with a handful of tables; they go together when the branch is torn down.
        m_d3d11.snapshot.store(owned.get(), std::memory_order_release);
        m_d3d11.history.push_back(std::move(owned));
    }

    void D3D11TextureManager::insert_owned(void *key, uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_d3d11.mutex);
        insert_owned_locked(key, hash);
    }

    void D3D11TextureManager::insert_owned_locked(void *key, uint32_t hash)
    {
        const D3D11State::OwnedSet *current = m_d3d11.snapshot.load(std::memory_order_acquire);
        if (current == nullptr || (current->count + 1) * 2 > current->mask)
        {
            size_t capacity = 64;
            while (capacity < (current != nullptr ? current->count + 1 : size_t{1}) * 4)
                capacity <<= 1;
            grow_locked(capacity, true);
            current = m_d3d11.snapshot.load(std::memory_order_acquire);
        }

        D3D11State::OwnedSet *owned = const_cast<D3D11State::OwnedSet *>(current);
        const size_t i = probe_slot(*owned, key);
        if (owned->slots[i].load(std::memory_order_relaxed) == key)
        {
            owned->hashes[i] = hash;
            return;
        }

        // The hash lands before the pointer that makes the slot visible, so a reader that sees the
        // pointer sees a hash that belongs to it.
        owned->hashes[i] = hash;
        owned->last_used[i].store(0, std::memory_order_relaxed);
        owned->overrides[i].store(nullptr, std::memory_order_relaxed);
        owned->slots[i].store(key, std::memory_order_release);
        owned->count++;
    }

    void D3D11TextureManager::forget_owned(void *key)
    {
        const D3D11State::OwnedSet *current = m_d3d11.snapshot.load(std::memory_order_acquire);
        if (current == nullptr || key == nullptr)
            return;

        // Looked up before the lock is taken: most views the game creates were never ours, and
        // taking a mutex to discover that would put one on every CreateShaderResourceView.
        D3D11State::OwnedSet *owned = const_cast<D3D11State::OwnedSet *>(current);
        const size_t i = probe_slot(*owned, key);
        if (owned->slots[i].load(std::memory_order_relaxed) != key)
            return;

        std::lock_guard<std::mutex> lock(m_d3d11.mutex);
        m_d3d11.views.erase(key);

        // The pointer stays in place: open addressing puts later keys behind it, and removing it
        // would strand them. Zeroing what it names is enough to make it answer "not ours".
        owned->overrides[i].store(nullptr, std::memory_order_release);
        owned->hashes[i] = 0;
    }

    void D3D11TextureManager::reset_owned()
    {
        std::lock_guard<std::mutex> lock(m_d3d11.mutex);
        grow_locked(64, false);
    }

    uint32_t D3D11TextureManager::note_referenced(void *resource, void *&override_out)
    {
        const D3D11State::OwnedSet *owned = m_d3d11.snapshot.load(std::memory_order_acquire);
        if (owned == nullptr || resource == nullptr)
            return 0;

        const size_t i = probe_slot(*owned, resource);
        if (owned->slots[i].load(std::memory_order_relaxed) != resource)
            return 0;

        owned->last_used[i].store(frame_ticks(), std::memory_order_relaxed);
        override_out = owned->overrides[i].load(std::memory_order_acquire);
        return owned->hashes[i];
    }

    void D3D11TextureManager::register_owned_view(void *view, uint32_t hash)
    {
        if (view == nullptr)
            return;

        // An untracked view at an address we know means the game freed the old one and the system
        // handed the address out again. Clearing the slot matters more than skipping it: leaving
        // the old hash there would substitute that texture's replacement for whatever this is now.
        if (hash == 0)
        {
            forget_owned(view);
            return;
        }

        // insert_or_assign, not emplace: the game frees views and the system hands the same
        // address out again, so an address we already know may now belong to another texture.
        std::lock_guard<std::mutex> lock(m_d3d11.mutex);
        m_d3d11.views.insert_or_assign(view, hash);
        insert_owned_locked(view, hash);

        // A view created after the replacement was built has to inherit it, or the game would
        // draw the original through a view we never saw at the time.
        if (auto it = m_d3d11.override_views.find(hash); it != m_d3d11.override_views.end())
            apply_override_locked(hash, it->second.Get());
    }

    void D3D11TextureManager::drop_override(uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_d3d11.mutex);
        if (m_d3d11.override_views.erase(hash) != 0)
            apply_override_locked(hash, nullptr);
    }

    void D3D11TextureManager::apply_override_locked(uint32_t hash, ID3D11ShaderResourceView *view)
    {
        const D3D11State::OwnedSet *current = m_d3d11.snapshot.load(std::memory_order_acquire);
        if (current == nullptr || hash == 0)
            return;

        D3D11State::OwnedSet *owned = const_cast<D3D11State::OwnedSet *>(current);
        for (size_t i = 0; i <= owned->mask; ++i)
        {
            void *slot = owned->slots[i].load(std::memory_order_relaxed);

            // Not onto the replacement's own view: it would substitute itself for itself.
            if (slot == nullptr || slot == view || owned->hashes[i] != hash)
                continue;

            owned->overrides[i].store(view, std::memory_order_release);
        }
    }

    void D3D11TextureManager::pin_preview_view(ID3D11ShaderResourceView *view)
    {
        std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
        if (lock.owns_lock() && m_d3d11.preview == nullptr)
            m_d3d11.preview = view;
    }

    void D3D11TextureManager::note_dump_candidate(ID3D11ShaderResourceView *view, uint32_t hash)
    {
        std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;

        if (!take_pending_dump(hash))
            return;

        view->AddRef();
        queue_readback(hash, nullptr, view);
    }

    bool D3D11TextureManager::owns_resource(ID3D11Resource *resource) const
    {
        return resource_hash(resource) != 0;
    }

    uint32_t D3D11TextureManager::resource_hash(ID3D11Resource *resource) const
    {
        if (resource == nullptr)
            return 0;

        uint32_t hash = 0;
        UINT size = sizeof(hash);
        if (FAILED(resource->GetPrivateData(TT_HASH_GUID, &size, &hash)) || size != sizeof(hash))
            return 0;

        return hash;
    }

    ID3D11Texture2D *D3D11TextureManager::create_replacement_texture11(ID3D11Device *device,
                                                                      D3D11_TEXTURE2D_DESC &desc,
                                                                      const D3D11_SUBRESOURCE_DATA &initial_data)
    {
        if (device == nullptr || !enable_injection)
            return nullptr;

        size_t row_bytes = 0, rows = 0;
        if (!tight_rows(desc.Format, desc.Width, desc.Height, row_bytes, rows))
            return nullptr;

        const uint32_t hash = hash_and_account(*this, initial_data.pSysMem, initial_data.SysMemPitch, row_bytes, rows);

        std::filesystem::path inject_path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto cached = m_d3d11.injected.find(hash);
            if (cached != m_d3d11.injected.end() && cached->second)
            {
                cached->second->AddRef();
                return cached->second.Get();
            }

            if (m_failed_injections.find(hash) != m_failed_injections.end())
                return nullptr;

            inject_path = find_injection_path_locked(hash);
            if (inject_path.empty())
                return nullptr;
        }

        DirectX::ScratchImage image;
        DirectX::TexMetadata meta = {};
        if (FAILED(DirectX::LoadFromDDSFile(inject_path.wstring().c_str(), DirectX::DDS_FLAGS_NONE, &meta, image)))
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_failed_injections.insert(hash);
            return nullptr;
        }

        if (desc.MipLevels != 1 && meta.mipLevels == 1)
        {
            DirectX::ScratchImage mipped;
            if (SUCCEEDED(DirectX::GenerateMipMaps(*image.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, mipped)))
            {
                image = std::move(mipped);
                meta = image.GetMetadata();
            }
        }

        ID3D11Resource *resource = nullptr;
        HRESULT hr = E_FAIL;
        {
            ScopedFlag injecting(D3D11Hook::s_inside_injection);
            hr = DirectX::CreateTexture(device, image.GetImages(), image.GetImageCount(), meta, &resource);
        }

        ID3D11Texture2D *texture = nullptr;
        if (SUCCEEDED(hr) && resource != nullptr)
        {
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
            resource->Release();
        }

        if (FAILED(hr) || texture == nullptr)
        {
            Logger::get().error(Logger::fmt(
                "[D3D11Textures] Injection failed for 0x%s, HRESULT: 0x%08X%s (%ux%u -> %zux%zu)",
                format_hash_hex(hash).c_str(), static_cast<unsigned>(hr), hresult_name(hr),
                desc.Width, desc.Height, meta.width, meta.height));
            if (texture != nullptr)
                texture->Release();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_failed_injections.insert(hash);
            return nullptr;
        }

        // Hand the engine back a descriptor that matches what it actually received: it keeps this
        // struct and builds its shader resource view from it.
        const D3D11_TEXTURE2D_DESC original = desc;
        texture->GetDesc(&desc);

        // Immutable would make the texture impossible to reload later.
        if (desc.Usage == D3D11_USAGE_IMMUTABLE)
            desc.Usage = D3D11_USAGE_DEFAULT;

        // This tag is how Hooked_CreateShaderResourceView recognises one of ours.
        texture->SetPrivateData(TT_HASH_GUID, sizeof(hash), &hash);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_d3d11.injected[hash] = texture;
            insert_owned(texture, hash);

            TextureDetails details;
            details.hash = hash;
            details.hash_hex = format_hash_hex(hash);
            details.width = original.Width;
            details.height = original.Height;
            details.mip_levels = original.MipLevels;
            details.format_id = static_cast<uint32_t>(original.Format);
            details.format_str = "DXGI_FORMAT_" + format_name(original.Format);
            details.format_short = "DX11_" + format_name(original.Format);
            details.is_compressed = DirectX::IsCompressed(original.Format);
            details.is_srgb = DirectX::IsSRGB(original.Format);
            details.is_dx11 = true;
            details.array_size = (original.ArraySize > 0) ? original.ArraySize : 1;
            details.bind_flags = original.BindFlags;
            details.misc_flags = original.MiscFlags;
            details.cpu_access = original.CPUAccessFlags;
            details.usage = static_cast<uint32_t>(original.Usage);
            details.status = TextureStatus::INJECTED;
            details.filepath_injected = inject_path.string();
            details.repl_width = desc.Width;
            details.repl_height = desc.Height;
            // replacement_handle is left at 0 on purpose: the panel feeds it straight to ImGui,
            // which needs a shader resource view, and what we hold is the texture. The preview
            // comes from the view the game binds.
            details.data_size = texture_byte_size(desc.Format, desc.Width, desc.Height, desc.MipLevels);
            details.last_seen_ticks = now_ticks();
            track(hash, details);
            stat_builds.fetch_add(1, std::memory_order_relaxed);
        }

        Logger::get().info(Logger::fmt("[D3D11Textures] Injected 0x%s (%ux%u fmt %u -> %ux%u fmt %u, %u mips)",
                                       format_hash_hex(hash).c_str(),
                                       original.Width, original.Height, static_cast<unsigned>(original.Format),
                                       desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                                       desc.MipLevels));
        return texture;
    }

    uint32_t D3D11TextureManager::register_texture11(ID3D11Device *device, ID3D11Resource *resource, const void *pixel_data,
                                                     UINT width, UINT height, DXGI_FORMAT format, UINT pitch,
                                                     const D3D11_SUBRESOURCE_DATA *initial_data, UINT mip_levels)
    {
        if (device == nullptr || resource == nullptr || pixel_data == nullptr || width == 0 || height == 0)
            return 0;

        if (filter_small_textures && (width < 16 || height < 16))
            return 0;

        size_t row_bytes = 0, rows = 0;
        if (!tight_rows(format, width, height, row_bytes, rows))
            return 0;

        const uint32_t hash = hash_and_account(*this, pixel_data, pitch, row_bytes, rows);
        if (hash == 0)
            return 0;

        D3D11_TEXTURE2D_DESC orig_desc = {};
        {
            ID3D11Texture2D *orig_tex = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&orig_tex))) && orig_tex != nullptr)
            {
                orig_tex->GetDesc(&orig_desc);
                orig_tex->Release();
            }
        }
        const UINT original_levels = orig_desc.MipLevels; // 0 = full chain generated by the runtime

        std::lock_guard<std::mutex> lock(m_mutex);

        resource->SetPrivateData(TT_HASH_GUID, sizeof(hash), &hash);

        TextureDetails details;
        details.hash = hash;
        details.hash_hex = format_hash_hex(hash);
        details.width = width;
        details.height = height;
        details.mip_levels = (original_levels == 0) ? full_mip_count(width, height) : original_levels;
        details.format_id = static_cast<uint32_t>(format);
        details.format_str = "DXGI_FORMAT_" + format_name(format);
        details.format_short = "DX11_" + format_name(format);
        details.is_compressed = DirectX::IsCompressed(format);
        details.is_srgb = DirectX::IsSRGB(format);
        details.is_dx11 = true;
        details.array_size = (orig_desc.ArraySize > 0) ? orig_desc.ArraySize : 1;
        details.bind_flags = orig_desc.BindFlags;
        details.misc_flags = orig_desc.MiscFlags;
        details.cpu_access = orig_desc.CPUAccessFlags;
        details.usage = static_cast<uint32_t>(orig_desc.Usage);
        details.data_size = texture_byte_size(format, width, height, details.mip_levels);
        details.last_seen_ticks = now_ticks();

        track(hash, details);

        // A file exists but this texture did not get a replacement when it was created -- it was
        // created empty and filled later, or it is not a shader resource, or its usage ruled it
        // out. Queue a replacement view instead; nothing here waits for Reload.
        note_pending_injection(hash);

        if (auto_dump)
        {
            // Every level the game uploaded, not just the first: this is the one moment the whole
            // chain is in hand, and a dump taken from it matches what the game loaded.
            DumpLevels levels;
            for (UINT level = 0; level < mip_levels; ++level)
            {
                const void *pixels = (initial_data != nullptr) ? initial_data[level].pSysMem : pixel_data;
                const UINT row = (initial_data != nullptr) ? initial_data[level].SysMemPitch : pitch;
                const UINT h = (std::max)(1u, height >> level);
                levels.push_back(copy_level(format, h, pixels, row));
                if (initial_data == nullptr)
                    break;
            }

            // The status follows the file: the writer thread sets it once the DDS is on disk.
            dump_texture(hash, width, height, format, std::move(levels));
        }

        return hash;
    }

    // A file that turned up for a hash the game has already created its texture for. Copying into
    // that texture is what refresh_injected_contents does below, and it is limited to a file of the
    // same shape -- and refused outright on the immutable usage most game art is created with. So
    // the original is left alone and a view of our own is put in front of it at bind time instead,
    // which is what Special K's D3D9 texture wrapper does one level lower down.
    //
    // One per frame, off the draw call that wanted it: reading a DDS and creating a texture inside
    // Present is the hitch this design exists to avoid.
    void D3D11TextureManager::process_branch_injections()
    {
        ID3D11Device *device = RenderBackend::get().d3d11_device();
        if (device == nullptr)
            return;

        uint32_t hash = 0;
        {
            if (!m_pending_injections.empty())
            {
                hash = *m_pending_injections.begin();
                m_pending_injections.erase(m_pending_injections.begin());
            }
        }

        if (hash == 0)
            return;

        const std::filesystem::path path = find_injection_path_locked(hash);
        if (path.empty())
            return;

        DirectX::ScratchImage image;
        DirectX::TexMetadata meta = {};
        if (!load_dds(path.string(), true, image, meta))
        {
            m_failed_injections.insert(hash);
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        {
            ScopedFlag injecting(D3D11Hook::s_inside_injection);

            if (FAILED(DirectX::CreateTexture(device, image.GetImages(), image.GetImageCount(), meta, &resource)) ||
                resource == nullptr)
            {
                m_failed_injections.insert(hash);
                return;
            }

            // The game's own view format decides sRGB, not the file: it picked how to sample this
            // texture and the replacement is standing in for it.
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            vd.Format = meta.format;
            if (auto it = m_tracked_textures.find(hash); it != m_tracked_textures.end() && it->second.view_format_id != 0)
            {
                const DXGI_FORMAT wanted = static_cast<DXGI_FORMAT>(it->second.view_format_id);
                vd.Format = DirectX::IsSRGB(wanted) ? DirectX::MakeSRGB(meta.format) : meta.format;
            }
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = static_cast<UINT>(-1);

            if (FAILED(device->CreateShaderResourceView(resource.Get(), &vd, &view)) || view == nullptr)
            {
                m_failed_injections.insert(hash);
                return;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_d3d11.mutex);
            m_d3d11.override_views[hash] = view;
            apply_override_locked(hash, view.Get());
        }

        if (auto it = m_tracked_textures.find(hash); it != m_tracked_textures.end())
        {
            it->second.status = TextureStatus::INJECTED;
            it->second.filepath_injected = path.string();
            it->second.repl_width = static_cast<uint32_t>(meta.width);
            it->second.repl_height = static_cast<uint32_t>(meta.height);
        }

        Logger::get().info("[D3D11Textures] Applied 0x" + format_hash_hex(hash) +
                           " to a texture the game had already created (" +
                           std::to_string(meta.width) + "x" + std::to_string(meta.height) + ").");
    }

    // The game is already holding these textures and will never ask for them again, so the new file
    // has to be copied into the texture it already has. Special K solves it the same way, which is
    // why it refuses immutable usage on them.
    size_t D3D11TextureManager::refresh_injected_contents()
    {
        ID3D11Device *device = RenderBackend::get().d3d11_device();
        ID3D11DeviceContext *context = RenderBackend::get().d3d11_context();
        if (device == nullptr || context == nullptr)
            return 0;

        std::vector<std::pair<uint32_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>>> live;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            live.assign(m_d3d11.injected.begin(), m_d3d11.injected.end());

            // A file for a hash the game created before this file existed: nothing to copy into,
            // so it is queued for a replacement view instead, one per frame.
            for (const auto &[hash, details] : m_tracked_textures)
                note_pending_injection(hash);
        }

        size_t refreshed = 0;
        for (const auto &[hash, texture] : live)
        {
            const std::filesystem::path path = find_injection_path(hash);
            if (path.empty())
                continue;

            DirectX::ScratchImage image;
            DirectX::TexMetadata meta = {};
            if (FAILED(DirectX::LoadFromDDSFile(path.wstring().c_str(), DirectX::DDS_FLAGS_NONE, &meta, image)))
                continue;

            D3D11_TEXTURE2D_DESC desc = {};
            texture->GetDesc(&desc);

            if (desc.MipLevels != 1 && meta.mipLevels == 1)
            {
                DirectX::ScratchImage mipped;
                if (SUCCEEDED(DirectX::GenerateMipMaps(*image.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, mipped)))
                {
                    image = std::move(mipped);
                    meta = image.GetMetadata();
                }
            }

            // CopyResource demands an exact match, so an edit that changes size or format cannot be
            // applied to a texture the game is already using.
            if (meta.width != desc.Width || meta.height != desc.Height ||
                meta.format != desc.Format || meta.mipLevels != desc.MipLevels)
            {
                Logger::get().warn(Logger::fmt(
                    "[D3D11Textures] 0x%s changed shape (%ux%u fmt %u -> %zux%zu fmt %u); restart the game to apply it.",
                    format_hash_hex(hash).c_str(), desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                    meta.width, meta.height, static_cast<unsigned>(meta.format)));
                continue;
            }

            ID3D11Resource *fresh = nullptr;
            ScopedFlag injecting(D3D11Hook::s_inside_injection);
            const HRESULT hr = DirectX::CreateTexture(device, image.GetImages(), image.GetImageCount(), meta, &fresh);

            if (SUCCEEDED(hr) && fresh != nullptr)
            {
                context->CopyResource(texture.Get(), fresh);
                fresh->Release();
                ++refreshed;
            }
        }

        Logger::get().info("[D3D11Textures] Refreshed " + std::to_string(refreshed) + " injected texture(s) in place.");
        return refreshed;
    }

    std::string D3D11TextureManager::dump_resource11(uint32_t hash, ID3D11Resource *res)
    {
        ID3D11Device *device = RenderBackend::get().d3d11_device();
        ID3D11DeviceContext *ctx = RenderBackend::get().d3d11_context();
        if (device == nullptr || ctx == nullptr || res == nullptr)
            return {};

        // CaptureTexture handles the staging copy, the multisample resolve and every mip and array
        // slice; the hand-rolled version did one mip and failed outright on the other two.
        //
        // The guard stays set across it: the staging CreateTexture2D and Map it performs go through
        // our own hooks, and Hooked_Unmap would otherwise register the staging texture as content.
        DirectX::ScratchImage image;
        HRESULT hr = E_FAIL;
        {
            ScopedFlag injecting(D3D11Hook::s_inside_injection);
            hr = DirectX::CaptureTexture(device, ctx, res, image);
        }

        if (SUCCEEDED(hr))
        {
            // A TYPELESS .dds is neither viewable nor re-injectable.
            const DXGI_FORMAT concrete = resolve_typeless(image.GetMetadata().format);
            if (concrete != image.GetMetadata().format)
                image.OverrideFormat(concrete);

            const std::filesystem::path dds_path = dump_path_for(hash);

            hr = DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                                        DirectX::DDS_FLAGS_NONE, dds_path.wstring().c_str());
            if (SUCCEEDED(hr))
                return dds_path.string();
        }

        // A texture is dropped from the dump queue when it is read, so a silent failure here leaves
        // it looking untouched for the rest of the session with nothing to explain it.
        Logger::get().warn(Logger::fmt("[D3D11Textures] Dump failed for 0x%s: 0x%08X%s",
                                       format_hash_hex(hash).c_str(),
                                       static_cast<unsigned>(hr), hresult_name(hr)));
        return {};
    }

    std::string D3D11TextureManager::dump_selected(uint32_t hash)
    {
        if (m_d3d11.preview == nullptr)
            return {};

        ID3D11Resource *res = nullptr;
        m_d3d11.preview->GetResource(&res);
        if (res == nullptr)
            return {};

        const std::string path = dump_resource11(hash, res);
        res->Release();
        return path;
    }

    uint64_t D3D11TextureManager::branch_preview_handle() const
    {
        return reinterpret_cast<uint64_t>(m_d3d11.preview.Get());
    }

    uint64_t D3D11TextureManager::upload_file_preview(const DirectX::Image &image)
    {
        ID3D11Device *dev = RenderBackend::get().d3d11_device();
        if (dev == nullptr || image.pixels == nullptr)
            return 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(image.width);
        desc.Height = static_cast<UINT>(image.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = resolve_typeless(image.format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = image.pixels;
        sd.SysMemPitch = static_cast<UINT>(image.rowPitch);
        sd.SysMemSlicePitch = static_cast<UINT>(image.slicePitch);

        ID3D11Texture2D *tex = nullptr;
        ScopedFlag injecting(D3D11Hook::s_inside_injection);
        HRESULT hr = dev->CreateTexture2D(&desc, &sd, &tex);
        if (SUCCEEDED(hr) && tex != nullptr)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
            svd.Format = desc.Format;
            svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MipLevels = 1;
            hr = dev->CreateShaderResourceView(tex, &svd, &m_d3d11.file_preview);
            tex->Release();
        }

        return reinterpret_cast<uint64_t>(m_d3d11.file_preview.Get());
    }

    void D3D11TextureManager::release_branch_replacements()
    {
        m_d3d11.injected.clear();
        m_d3d11.views.clear();
        reset_owned();

        // After reset_owned: the table is what the render thread reads, and dropping these views
        // while a slot still points at one would leave it reading a freed interface.
        m_d3d11.override_views.clear();
    }

    void D3D11TextureManager::release_branch_file_preview()
    {
        m_d3d11.file_preview.Reset();
    }

    void D3D11TextureManager::release_branch_preview()
    {
        m_d3d11.preview.Reset();
        m_d3d11.file_preview.Reset();
    }

    void D3D11TextureManager::collect_scene_hashes(uint64_t now, std::unordered_set<uint32_t> &out)
    {
        const D3D11State::OwnedSet *owned = m_d3d11.snapshot.load(std::memory_order_acquire);
        if (owned == nullptr)
            return;

        // A texture stays listed for a while after its last bind: the game does not bind every
        // texture every frame, and a list that blinks is worse than one that lags.
        const uint64_t window = static_cast<uint64_t>(kSceneLingerSeconds * static_cast<double>(ticks_per_second()));
        const uint64_t cutoff = now > window ? now - window : 1;

        for (size_t i = 0; i <= owned->mask; ++i)
        {
            if (owned->slots[i].load(std::memory_order_acquire) != nullptr &&
                owned->last_used[i].load(std::memory_order_relaxed) >= cutoff)
                out.insert(owned->hashes[i]);
        }
    }

    uint64_t D3D11TextureManager::branch_file_preview_handle() const
    {
        return reinterpret_cast<uint64_t>(m_d3d11.file_preview.Get());
    }

    std::string D3D11TextureManager::dump_readback(const PendingReadback &rb)
    {
        if (rb.srv11 == nullptr)
            return {};

        ID3D11Resource *res = nullptr;
        rb.srv11->GetResource(&res);
        if (res == nullptr)
            return {};

        // The queue holds a pointer the game may have freed since, and a freed address gets handed
        // out again. The tag on the resource is what it actually is.
        const uint32_t actual = resource_hash(res);
        const std::string path = (actual != 0) ? dump_resource11(actual, res) : std::string();
        res->Release();
        return path;
    }
}
