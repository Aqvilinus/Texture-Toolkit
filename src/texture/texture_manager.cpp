#include <charconv>
#include "texture/texture_manager.h"
#include "render/d3d11/d3d11_textures.h"
#include "render/d3d9/d3d9_textures.h"
#include "render/render_backend.h"
#include "render/d3d9/d3d9_format.h"
#include "render/dxgi/dxgi_format.h"
#include "core/config.h"
#include "render/d3d9/d3d9_hook.h"
#include "render/d3d11/d3d11_hook.h"
#include <DirectXTex.h>
#include <objbase.h>
#include "core/logger.h"
#include "ui/overlay.h"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace TextureToolkit
{
    constexpr double kEvictAgeSeconds = 60.0;
    TextureManagerBase *TextureManagerBase::active()
    {
        RenderBackend &backend = RenderBackend::get();
        if (backend.is_d3d11())
            return &D3D11TextureManager::get();
        if (backend.is_d3d9())
            return &D3D9TextureManager::get();
        return nullptr;
    }

    void TextureManagerBase::track(uint32_t hash, TextureDetails details)
    {
        // The game creates the same content many times over a session, and each registration
        // describes only the new instance -- it knows nothing of the dump an earlier one wrote.
        // Carry that across, or the panel forgets a file that is sitting on disk. The fresh status
        // wins when it says INJECTED: that describes this instance, and a replacement may have been
        // released since the last one.
        if (auto old = m_tracked_textures.find(hash); old != m_tracked_textures.end())
        {
            if (details.filepath_dumped.empty())
                details.filepath_dumped = old->second.filepath_dumped;
            if (details.status == TextureStatus::ORIGINAL && !details.filepath_dumped.empty())
                details.status = TextureStatus::DUMPED;
        }

        m_tracked_textures.insert_or_assign(hash, std::move(details));
    }

    void TextureManagerBase::set_status(uint32_t hash, TextureStatus status)
    {
        auto it = m_tracked_textures.find(hash);
        if (it != m_tracked_textures.end())
            it->second.status = status;
    }

    void TextureManagerBase::queue_pending_dump(uint32_t hash)
    {
        m_pending_dumps.insert(hash);
        m_dumps_pending.store(true, std::memory_order_relaxed);
    }

    bool TextureManagerBase::take_pending_dump(uint32_t hash)
    {
        if (m_pending_dumps.erase(hash) == 0)
            return false;

        m_dumps_pending.store(!m_pending_dumps.empty(), std::memory_order_relaxed);
        return true;
    }

    void TextureManagerBase::queue_readback(uint32_t hash, IDirect3DBaseTexture9 *tex9, ID3D11ShaderResourceView *srv11)
    {
        m_readback_queue.push_back({hash, tex9, srv11});
    }

    void TextureManagerBase::init()
    {
        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));

        m_game_dir = std::filesystem::path(exe_path).parent_path();

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

        m_dump_thread = std::jthread([this](std::stop_token stop) { dump_worker_loop(stop); });

    }

    void TextureManagerBase::shutdown()
    {
        // Joined here rather than left to the destructor: the worker touches the state that is
        // released just below.
        m_dump_thread.request_stop();
        if (m_dump_thread.joinable())
            m_dump_thread.join();

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

    void TextureManagerBase::set_preview_target(uint32_t hash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (hash == m_preview_target_hash)
            return;
        release_preview();
        m_preview_target_hash = hash;
        m_preview_wanted.store(hash, std::memory_order_relaxed);
    }

    uint64_t TextureManagerBase::get_original_preview_handle()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return branch_preview_handle();
    }

    // Caller MUST hold m_mutex.
    void TextureManagerBase::release_preview()
    {
        release_branch_preview();
        m_file_preview_hash = 0;
    }

    uint64_t TextureManagerBase::get_file_preview_handle(uint32_t hash, const std::string &dds_path)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::error_code ec;
        const std::filesystem::file_time_type written = std::filesystem::last_write_time(dds_path, ec);
        if (ec)
            return 0;

        // The timestamp is part of the key: overwriting <hash>.dds keeps the name, and the panel
        // would otherwise go on showing the texture uploaded from the previous contents.
        if (hash == m_file_preview_hash && written == m_file_preview_written)
            return branch_file_preview_handle();

        release_branch_file_preview();
        m_file_preview_hash = 0;

        DirectX::ScratchImage image;
        DirectX::TexMetadata meta = {};
        if (!load_dds(dds_path, false, image, meta))
            return 0;

        const DirectX::Image *top = image.GetImage(0, 0, 0);
        if (top == nullptr)
            return 0;

        // Only now: a file half-written when we first looked at it must be retried, and the key
        // is what says we already have this one.
        const uint64_t handle = upload_file_preview(*top);
        if (handle != 0)
        {
            m_file_preview_hash = hash;
            m_file_preview_written = written;
        }
        return handle;
    }

    void TextureManagerBase::release_replacements()
    {
        release_branch_replacements();
    }

    void TextureManagerBase::on_frame()
    {
        // try_lock, not lock: the build thread holds m_mutex in short bursts, and a frame that
        // waits on it is a stutter. Everything below is bookkeeping for the panel and can just
        // as well happen next frame.
        std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;

        m_frame_count++;

        const uint64_t now = now_ticks();
        m_frame_ticks.store(now, std::memory_order_relaxed);

        // swap, not copy: assigning an unordered_set rehashes and reallocates on the frame path.
        m_active_frame_hashes.swap(m_current_frame_hashes);
        m_current_frame_hashes.clear();

        collect_scene_hashes(now, m_active_frame_hashes);


        for (uint32_t hash : m_active_frame_hashes)
        {
            auto it = m_tracked_textures.find(hash);
            if (it != m_tracked_textures.end())
                it->second.last_seen_ticks = now;
        }

        if (m_frame_count % 300 == 0)
            evict_stale_textures();

        process_branch_injections();
        lock.unlock();

        process_readback_queue();
    }

    void TextureManagerBase::evict_stale_textures()
    {
        // Injected, selected and inject-file hashes survive regardless of age: those must never
        // vanish from the panel.
        const uint64_t age = static_cast<uint64_t>(kEvictAgeSeconds * static_cast<double>(ticks_per_second()));
        const uint64_t now = now_ticks();
        if (now < age)
            return;

        for (auto it = m_tracked_textures.begin(); it != m_tracked_textures.end();)
        {
            const TextureDetails &d = it->second;
            bool stale = d.last_seen_ticks + age < now;
            bool keep = d.replacement_handle != 0 ||
                        it->first == m_preview_target_hash ||
                        m_injected_files.find(it->first) != m_injected_files.end();

            if (stale && !keep)
                it = m_tracked_textures.erase(it);
            else
                ++it;
        }
    }

    size_t TextureManagerBase::rescan_injected()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_injected_files.clear();
        m_failed_injections.clear(); // retry files that were bad last time; they may be fixed now
        m_pending_injections.clear();

        std::error_code scan_ec;
        if (!std::filesystem::exists(m_inject_dir, scan_ec) || scan_ec)
            return 0;

        for (const auto &entry : std::filesystem::directory_iterator(m_inject_dir, scan_ec))
        {
            if (!entry.is_regular_file(scan_ec) || scan_ec)
                continue;

            std::string ext = entry.path().extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            // DDS-only injection for maximum format/compatibility safety.
            if (ext != ".dds")
                continue;

            const std::string stem = entry.path().stem().string();
            std::string_view digits{stem};
            if (digits.starts_with("0x") || digits.starts_with("0X"))
                digits.remove_prefix(2);

            // from_chars rather than stoul: no exception, no allocation, and it refuses a name with
            // anything after the hex instead of silently taking the prefix.
            uint32_t hash = 0;
            const auto [last, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), hash, 16);
            if (ec == std::errc{} && last == digits.data() + digits.size())
                m_injected_files.insert_or_assign(hash, entry.path());
        }

        Logger::get().info("[TextureManager] Scanned " + std::to_string(m_injected_files.size()) + " DDS replacement file(s) in TT/inject.");
        lock.unlock();

        return refresh_branch();
    }

    std::filesystem::path TextureManagerBase::find_injection_path(uint32_t hash)
    {
        auto it = m_injected_files.find(hash);
        if (it != m_injected_files.end())
        {
            return it->second;
        }
        return std::filesystem::path();
    }

    // Total free address space and the largest single free block. A 32-bit process usually fails
    // an allocation for want of a contiguous range, not for want of memory; only the second
    uint64_t TextureManagerBase::ticks_per_second()
    {
        static const uint64_t frequency = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return static_cast<uint64_t>(f.QuadPart);
        }();
        return frequency;
    }

    uint64_t TextureManagerBase::now_ticks()
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<uint64_t>(counter.QuadPart);
    }

    std::vector<TextureDetails> TextureManagerBase::get_active_textures()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<TextureDetails> result;
        result.reserve(m_tracked_textures.size());

        const uint64_t now = now_ticks();
        const uint64_t scene_window = static_cast<uint64_t>(kSceneLingerSeconds * static_cast<double>(ticks_per_second()));

        for (const auto &pair : m_tracked_textures)
        {
            if (show_current_frame_only)
            {
                if (pair.second.last_seen_ticks == 0 || pair.second.last_seen_ticks + scene_window < now)
                    continue;
            }

            TextureDetails entry = pair.second;

            // A replacement can appear after the texture was tracked -- dropping a file in and
            // hitting Reload -- so injection is decided here. From the replacement the branch
            // actually built, not from a file lying in inject/: that file may be corrupt, in a
            // format the device refuses, or ignored entirely with EnableInjection off. Into the
            // copy being handed out, because a reader has no business rewriting what it reads.
            if (entry.replacement_handle != 0 || branch_has_replacement(pair.first))
                entry.status = TextureStatus::INJECTED;

            result.push_back(std::move(entry));
        }
        return result;
    }

}
