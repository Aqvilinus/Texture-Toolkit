#include "texture/texture_manager.h"
#include "render/dxgi/dxgi_format.h"
#include "texture/texture_hash.h"
#include "core/logger.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cstring>

namespace TextureToolkit
{
    DirectX::ScratchImage TextureManagerBase::make_dump_image(DXGI_FORMAT format, UINT width, UINT height, UINT mip_levels)
    {
        DirectX::ScratchImage image;

        // A TYPELESS .dds is neither viewable nor re-injectable, so the concrete format is decided
        // here, before anything is copied in.
        DirectX::TexMetadata meta = {};
        meta.width = width;
        meta.height = height;
        meta.depth = 1;
        meta.arraySize = 1;
        meta.mipLevels = (mip_levels == 0) ? 1 : mip_levels;
        meta.format = resolve_typeless(format);
        meta.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

        if (width == 0 || height == 0 || FAILED(image.Initialize(meta)))
            image.Release();

        return image;
    }

    bool TextureManagerBase::copy_level(DirectX::ScratchImage &image, size_t level, const void *pixels, size_t row_pitch)
    {
        const DirectX::Image *dst = image.GetImage(level, 0, 0);
        if (dst == nullptr || dst->pixels == nullptr || pixels == nullptr || row_pitch == 0)
            return false;

        // A source row shorter than the format demands cannot be padded into a correct image, and
        // the old code's min() quietly wrote one anyway.
        if (row_pitch < dst->rowPitch)
            return false;

        const size_t rows = DirectX::ComputeScanlines(dst->format, dst->height);
        const uint8_t *src = static_cast<const uint8_t *>(pixels);
        uint8_t *out = dst->pixels;

        for (size_t row = 0; row < rows; ++row)
        {
            std::memcpy(out, src, dst->rowPitch);
            src += row_pitch;
            out += dst->rowPitch;
        }

        return true;
    }

    bool TextureManagerBase::dump_texture(uint32_t hash, DirectX::ScratchImage &&image)
    {
        if (hash == 0 || image.GetImageCount() == 0)
            return false;

        DumpRequest req;
        req.hash = hash;
        req.image = std::move(image);

        {
            std::lock_guard<std::mutex> lock(m_dump_mutex);
            m_dump_queue.push_back(std::move(req));
        }
        m_dump_cv.notify_one();
        return true;
    }

    std::string TextureManagerBase::write_dump_dds(uint32_t hash, const DirectX::ScratchImage &image)
    {
        if (image.GetImageCount() == 0)
            return {};

        const std::filesystem::path dds_path = dump_path_for(hash);

        const HRESULT hr = DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                                                  DirectX::DDS_FLAGS_NONE, dds_path.wstring().c_str());
        if (FAILED(hr))
        {
            Logger::get().error(Logger::fmt("[TextureManager] Failed to write dump %s - 0x%08X%s",
                                            dds_path.string().c_str(), static_cast<unsigned>(hr),
                                            hresult_name(hr)));
            return {};
        }
        return dds_path.string();
    }

    // Named so it is identifiable among the game's own threads in a profiler. Resolved at runtime:
    // a direct call would put an import in the DLL that Windows 8 and older cannot satisfy, and the
    // whole library would fail to load there.
    static void name_current_thread(const wchar_t *name)
    {
        using SetThreadDescription_t = HRESULT(WINAPI *)(HANDLE, PCWSTR);

        static const auto set_description = reinterpret_cast<SetThreadDescription_t>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));

        if (set_description != nullptr)
            set_description(GetCurrentThread(), name);
    }

    void TextureManagerBase::dump_worker_loop(std::stop_token stop)
    {
        name_current_thread(L"TT Dump Writer");

        while (true)
        {
            DumpRequest req;
            {
                std::unique_lock<std::mutex> lock(m_dump_mutex);
                m_dump_cv.wait(lock, stop, [this]() { return !m_dump_queue.empty(); });

                if (m_dump_queue.empty()) // woken by the stop request
                    break;

                req = std::move(m_dump_queue.front());
                m_dump_queue.pop_front();
            }

            std::string path = write_dump_dds(req.hash, req.image);
            if (!path.empty())
            {
                note_dumped(req.hash, path);
            }
            else
            {
                Logger::get().error("[TextureManager] Failed to dump texture 0x" + format_hash_hex(req.hash));
            }
        }
    }

    void TextureManagerBase::process_readback_queue()
    {
        // Each readback stalls on the GPU and then writes to disk. Holding m_mutex across that
        // would fail the bind hook's try_lock for the whole frame -- and the bind hook is what
        // feeds this queue, so dumping would throttle itself.
        std::vector<PendingReadback> batch;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            int budget = 8;
            while (!m_readback_queue.empty() && budget-- > 0)
            {
                batch.push_back(m_readback_queue.front());
                m_readback_queue.pop_front();
            }
        }

        for (PendingReadback &rb : batch)
        {
            std::string path;
            // Which pointer is live depends on the branch, and so does how it is read back.
            path = dump_readback(rb);

            if (rb.srv11 != nullptr)
                rb.srv11->Release();
            if (rb.tex9 != nullptr)
                rb.tex9->Release();

            if (path.empty())
                continue;

            note_dumped(rb.hash, path);
        }
    }

    size_t TextureManagerBase::dump_all(bool scene_only)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t now = now_ticks();
        const uint64_t scene_window = static_cast<uint64_t>(kSceneLingerSeconds * static_cast<double>(ticks_per_second()));

        size_t queued = 0;
        for (auto &pair : m_tracked_textures)
        {
            if (scene_only)
            {
                const TextureDetails &d = pair.second;
                bool active = d.last_seen_ticks != 0 && d.last_seen_ticks + scene_window >= now;
                if (!active)
                    continue;
            }
            queue_pending_dump(pair.first);
            ++queued;
        }
        Logger::get().info("[TextureManager] Dump-all queued " + std::to_string(queued) + (scene_only ? " active" : " tracked") + " texture(s); each is written the next time it is drawn.");
        return queued;
    }

    bool TextureManagerBase::request_dump(uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_tracked_textures.find(hash);
        if (it == m_tracked_textures.end())
            return false;
        TextureDetails &d = it->second;

        // We read back only the live handle pinned while the texture is on screen. Reading
        // an arbitrary tracked pointer is unsafe (it may have been freed and its address
        // reused), so dumping requires the texture to be visible when the button is clicked.
        if (hash != m_preview_target_hash)
        {
            Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) + ": select the texture first.");
            return false;
        }

        const std::string path = dump_selected(hash);

        if (!path.empty())
        {
            d.filepath_dumped = path;
            if (d.status != TextureStatus::INJECTED)
                d.status = TextureStatus::DUMPED;
            Logger::get().info("[TextureManager] Dumped 0x" + format_hash_hex(hash) + " to " + path);
            return true;
        }

        Logger::get().warn("[TextureManager] Cannot dump 0x" + format_hash_hex(hash) +
                           ": it is not on screen, or cannot be read back on demand. Turn on Auto-dump to capture it from the upload instead.");
        return false;
    }

}
