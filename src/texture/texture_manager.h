#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <stop_token>
#include <atomic>
#include <memory>
#include <wrl/client.h>
#include <deque>
#include <filesystem>
#include "texture/texture_hash.h"
#include "render/dxgi/dxgi_format.h"

namespace TextureToolkit
{
    // Tags every original game resource with its content hash. The driver clears private data when
    // the object dies, so a reused pointer never carries a stale hash.
    // How long a texture stays listed as on screen after the last time the game bound it. The game
    // does not bind every texture every frame, so a shorter window blinks.
    inline constexpr double kSceneLingerSeconds = 1.0;

    inline constexpr GUID TT_HASH_GUID =
        { 0x6b7a4c10, 0x3f2e, 0x4d9a, { 0x9e, 0x21, 0x8c, 0x0a, 0x5b, 0x1d, 0x2e, 0x34 } };

    enum class TextureStatus
    {
        ORIGINAL = 0,
        INJECTED = 1,
        DUMPED = 2
    };

    struct TextureDetails
    {
        uint32_t hash = 0;
        std::string hash_hex;

        uint32_t width = 0;          // original game-texture dimensions
        uint32_t height = 0;
        uint32_t mip_levels = 1;

        uint32_t repl_width = 0;     // injected replacement dimensions (0 if not injected)
        uint32_t repl_height = 0;

        uint32_t data_size = 0;      // GPU byte size of the full mip chain

        uint32_t format_id = 0;      // native DXGI_FORMAT / D3DFORMAT value
        std::string format_str;      // full name, e.g. "DXGI_FORMAT_BC3_UNORM"
        std::string format_short;    // list label, e.g. "DX11_BC3_UNORM"
        bool is_compressed = false;
        bool is_srgb = false;
        bool is_dx11 = false;

        // How the game actually samples the texture (the SRV's view format). For a TYPELESS
        // resource this is where the concrete format and sRGB intent live. 0 until first drawn.
        uint32_t view_format_id = 0;
        std::string view_format_str;

        // DX11 resource description (0 / defaults on DX9).
        uint32_t array_size = 1;
        uint32_t bind_flags = 0;
        uint32_t misc_flags = 0;
        uint32_t cpu_access = 0;
        uint32_t usage = 0;

        TextureStatus status = TextureStatus::ORIGINAL;
        std::string filepath_dumped;
        std::string filepath_injected;

        uint64_t replacement_handle = 0;
        uint64_t last_seen_ticks = 0;
    };

    // Shared by both graphics APIs: the tracked-texture list keyed by content hash, the panel's
    // view of it, and the dump queue. What differs -- how a replacement is made and how a live
    // texture is read back -- is left to the branch classes below it.
    class TextureManagerBase
    {
    public:
        // Whichever branch the game turned out to use. A process realistically drives one API, so
        // the panel and the dump queue can address it without knowing which.
        static TextureManagerBase *active();

        void init();
        void shutdown();
        // Rescans TT/inject and refreshes the textures the game is already holding. Returns how
        // many were refreshed; files added for textures the game has not created yet apply the
        // next time it creates them.
        size_t rescan_injected();
        std::unordered_map<uint32_t, std::filesystem::path> scan_inject_dir() const;
        void on_frame();

        std::filesystem::path get_dump_dir() const { return m_dump_dir; }
        std::filesystem::path get_inject_dir() const { return m_inject_dir; }

        bool auto_dump = false;
        bool enable_injection = true;
        bool filter_small_textures = true;

        // Read and reset by the Present hook's periodic report.
        std::atomic<uint64_t> stat_builds{0};      // replacements built (DDS read + texture create)
        std::atomic<uint64_t> stat_hash_ticks{0};  // QPC ticks spent hashing textures on creation
        std::atomic<uint64_t> stat_hash_bytes{0};  // bytes fed to that hash
        bool show_current_frame_only = false;

        std::vector<TextureDetails> get_active_textures();

        // Files in inject/ and how many of them the branch could not build anything from. A failed
        // one leaves its texture looking untouched in the panel, which is indistinguishable from
        // never having had a file at all.
        struct InjectionSummary
        {
            size_t files = 0;
            size_t failed = 0;
        };
        InjectionSummary injection_summary();

        // How the game samples this texture, which is what reveals the sRGB intent a TYPELESS
        // resource hides. Called by the view-creation hook, so once per view rather than on every
        // bind the way upstream did it. Takes m_mutex.
        void note_view_format(uint32_t hash, uint32_t format_id);

        // Live original-texture preview. The UI names one hash as the preview target; the
        // next time that texture is bound we take a COM reference to the exact resource the
        // game is using, so the preview stays valid even under pointer reuse and cannot be
        // freed while shown. At most one original is pinned at a time. Returns 0 until the
        // target is bound (i.e. visible in the scene).
        void set_preview_target(uint32_t hash);
        uint64_t get_original_preview_handle();

        // Loads a dumped .dds from disk into a GPU texture for preview and caches it (one
        // at a time, released when the selection changes). Lets us show textures that are
        // not currently on screen. Returns a native texture id, or 0 on failure.
        uint64_t get_file_preview_handle(uint32_t hash, const std::string &dds_path);




        // Creation-time injection. Returns the replacement texture for the pixels the game is
        // about to upload, or nullptr when there is none and the game's own call should proceed.
        // `desc` is the game's own descriptor and is updated to the replacement's dimensions,
        // format and mip count: the engine builds its shader resource view from that same struct,
        // and a view whose format disagrees with the resource is refused by D3D11.


        // Reading back a texture needs a handle that is known to still be alive, and the only
        // moment that is guaranteed is while the game is binding it.
        // The one hash the panel is showing, or 0. Read once per bind call rather than asked per
        // view, since the answer cannot change inside one.
        uint32_t preview_wanted() const { return m_preview_wanted.load(std::memory_order_relaxed); }

        // A queued texture can only be read back while the game has it bound, so the bind hook is
        // where the queue is served.
        bool has_pending_dumps() const { return m_dumps_pending.load(std::memory_order_relaxed); }


        // Dumps a texture to TT/dump immediately (synchronously, on the calling thread) and
        // returns true on success. Reads back the live GPU resource: the pinned preview
        // reference when the texture is the current selection, otherwise the tracked handle.
        bool request_dump(uint32_t hash);

        // Queues a bulk dump. scene_only limits it to textures active this scene, otherwise
        // every tracked texture. Each is written the next time the game draws it (using the
        // live handle, so it is safe against pointer reuse). Returns the number queued.
        size_t dump_all(bool scene_only);

        // Queues an async dump from CPU pixel data already in hand (used by auto-dump).
        struct DumpLevel
        {
            std::vector<uint8_t> data;
            UINT row_pitch = 0;
        };
        using DumpLevels = std::vector<DumpLevel>;

        bool dump_texture(uint32_t hash, UINT width, UINT height, DXGI_FORMAT format, DumpLevels levels);

        // Copies one upload level into an owned buffer. The row count comes from the format, so a
        // block-compressed surface copies block rows rather than pixel rows.
        static DumpLevel copy_level(DXGI_FORMAT format, UINT height, const void *pixels, UINT row_pitch);


        struct PendingReadback
        {
            uint32_t hash = 0;
            IDirect3DBaseTexture9 *tex9 = nullptr;
            ID3D11ShaderResourceView *srv11 = nullptr;
        };

    protected:
        // Small guarded operations instead of the map itself: the branches run on the render
        // thread and must not be able to take m_mutex twice by accident.
        void track(uint32_t hash, TextureDetails details);             // m_mutex held
        void set_status(uint32_t hash, TextureStatus status);          // m_mutex held

        // Queues a hash for the one replacement built per frame, if a file exists for it, injection
        // is on, the file has not already failed, and the branch has not already built one. The
        // rule lived in three places and had begun to differ between them. m_mutex held.
        void note_pending_injection(uint32_t hash);

        // Where a dump for this hash goes, creating the folder if it is not there. The name is the
        // one scan_inject_dir has to be able to parse back, so it is built in one place.
        std::filesystem::path dump_path_for(uint32_t hash) const;

        // Records a written dump. Takes m_mutex, and leaves an injected texture reading as injected.
        void note_dumped(uint32_t hash, const std::string &path);
        bool take_pending_dump(uint32_t hash);
        void queue_pending_dump(uint32_t hash);                         // m_mutex held
        void queue_readback(uint32_t hash, IDirect3DBaseTexture9 *tex9, ID3D11ShaderResourceView *srv11);
        uint64_t frame_ticks() const { return m_frame_ticks.load(std::memory_order_relaxed); }

        virtual size_t refresh_branch() = 0;

        virtual uint64_t branch_file_preview_handle() const = 0;

        // Adds whatever this branch saw on screen since `now` minus the scene window.
        virtual void collect_scene_hashes(uint64_t now, std::unordered_set<uint32_t> &out) {}

        // Builds replacements flagged earlier; only the draw-time branch has any.
        virtual void process_branch_injections() {}

        virtual std::string dump_readback(const PendingReadback &rb) = 0;

        // Whether a replacement was actually built for this hash. m_mutex held.
        virtual bool branch_has_replacement(uint32_t hash) const = 0;

        TextureManagerBase() = default;

        std::filesystem::path m_game_dir;
        std::filesystem::path m_dump_dir;
        std::filesystem::path m_inject_dir;


        mutable std::mutex m_mutex;
        uint64_t m_next_eviction_ticks = 0;
        // Sampled once per frame and read by the bind hook, so "recently drawn" is a duration
        // rather than a frame count: the same 60 frames are a second at 60 fps and a third of one
        // at 160.
        std::atomic<uint64_t> m_frame_ticks{0};
        static uint64_t now_ticks();
        static uint64_t ticks_per_second();

        // Active-scene tracking is keyed by content hash (immune to driver pointer reuse).
        std::unordered_set<uint32_t> m_current_frame_hashes;
        std::unordered_set<uint32_t> m_active_frame_hashes;
        std::unordered_map<uint32_t, std::filesystem::path> m_injected_files;
        std::unordered_map<uint32_t, TextureDetails> m_tracked_textures;




        void release_replacements();






        std::unordered_set<uint32_t> m_pending_injections;

        // Hashes whose inject file failed to load/create, so we do not retry a broken file every
        // frame. Cleared by rescan_injected.
        std::unordered_set<uint32_t> m_failed_injections;

        // Live original-texture preview (see set_preview_target). We hold one COM reference.
        uint32_t m_preview_target_hash = 0;
        std::atomic<uint32_t> m_preview_wanted{0};   // m_preview_target_hash, readable without the lock

        uint32_t m_file_preview_hash = 0;
        std::filesystem::file_time_type m_file_preview_written{};

        void release_preview();

        struct DumpRequest
        {
            uint32_t hash = 0;
            UINT width = 0;
            UINT height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            DumpLevels levels;
        };

        std::deque<DumpRequest> m_dump_queue;
        std::jthread m_dump_thread;
        std::condition_variable_any m_dump_cv;
        std::mutex m_dump_mutex;

        void dump_worker_loop(std::stop_token stop);

        // Writes a single-mip DDS to TT/dump from CPU pixel data. Returns the file path, or
        // empty on failure. Does not touch tracked state; the caller updates status.
        std::string write_dump_dds(uint32_t hash, UINT width, UINT height, DXGI_FORMAT format, const DumpLevels &levels);


        // Bulk-dump plumbing (see dump_all). m_pending_dumps holds hashes waiting to be
        // drawn; when drawn we take a reference into m_readback_queue and drain it a few per
        // frame in on_frame. process_readback_queue locks m_mutex itself, in short bursts.
        std::unordered_set<uint32_t> m_pending_dumps;
        std::atomic<bool> m_dumps_pending{false};
        std::deque<PendingReadback> m_readback_queue;
        void process_readback_queue();

        // Drops long-unseen tracked textures so the map does not grow without bound over a
        // long session. Caller MUST hold m_mutex.
        void evict_stale_textures();

        std::filesystem::path find_injection_path_locked(uint32_t hash);   // m_mutex held
        std::filesystem::path find_injection_path(uint32_t hash);          // takes m_mutex


        // Handle of the live texture pinned for preview, 0 if none is pinned yet.
        virtual uint64_t branch_preview_handle() const = 0;
        virtual uint64_t upload_file_preview(const DirectX::Image &image) = 0;
        virtual std::string dump_selected(uint32_t hash) = 0;
        virtual void release_branch_replacements() = 0;
        // Drop the pinned live texture and the cached file preview.
        virtual void release_branch_preview() = 0;
        // Only the preview built from a file: the live pinned texture may be on screen right now.
        virtual void release_branch_file_preview() = 0;
    };
}
