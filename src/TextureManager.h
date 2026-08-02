#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include "TextureHash.h"

namespace TextureToolkit
{
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
        std::string hash_hex_0x;
        
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth_or_layers = 1;
        uint32_t mip_levels = 1;
        uint32_t samples = 1;

        uint32_t format_id = 0;
        std::string format_str;
        std::string format_short;
        bool is_compressed = false;
        bool is_srgb = false;

        TextureStatus status = TextureStatus::ORIGINAL;
        std::string filepath_dumped;
        std::string filepath_injected;

        uint64_t resource_handle = 0;
        uint64_t replacement_handle = 0;
        uint64_t last_seen_frame = 0;
    };

    class TextureManager
    {
    public:
        static TextureManager &get();

        void init();
        void shutdown();
        void rescan_injected();
        void on_frame();

        std::filesystem::path get_dump_dir() const { return m_dump_dir; }
        std::filesystem::path get_inject_dir() const { return m_inject_dir; }

        // Settings
        bool auto_dump = false;
        bool enable_injection = true;
        bool filter_small_textures = true;
        bool show_current_frame_only = false;

        uint64_t get_current_frame() const { return m_frame_count; }

        // Active Texture Queries
        std::vector<TextureDetails> get_active_textures();

        // Virtual Replacements for DX9
        IDirect3DBaseTexture9 *get_replacement_texture9(IDirect3DBaseTexture9 *orig);
        void register_unmap_texture9(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch);

        // Virtual Replacements for DX11
        ID3D11ShaderResourceView *get_replacement_srv11(ID3D11ShaderResourceView *orig);
        void register_unmap_texture11(ID3D11Device *device, ID3D11Resource *resource, const void *pixel_data, UINT width, UINT height, DXGI_FORMAT format, UINT pitch);

        // Dump Helpers
        void request_dump(uint32_t hash);
        bool dump_texture(uint32_t hash, UINT width, UINT height, DXGI_FORMAT format, const void *data, UINT row_pitch);

    private:
        TextureManager() = default;

        std::filesystem::path m_game_dir;
        std::filesystem::path m_dump_dir;
        std::filesystem::path m_inject_dir;

        mutable std::mutex m_mutex;
        uint64_t m_frame_count = 0;

        // Active-scene tracking is keyed by content hash (immune to driver pointer reuse).
        std::unordered_set<uint32_t> m_current_frame_hashes;
        std::unordered_set<uint32_t> m_active_frame_hashes;
        std::unordered_set<uint32_t> m_requested_dumps;
        std::unordered_map<uint32_t, std::filesystem::path> m_injected_files;
        std::unordered_map<uint32_t, TextureDetails> m_tracked_textures;

        // Replacement maps are keyed by content hash, not by raw resource pointer.
        // The original resource is tagged with its hash via D3D private data
        // (TT_HASH_GUID), which the driver clears when the object is destroyed, so a
        // reused pointer can never inherit a stale replacement.
        // We own exactly one COM reference for each stored replacement.
        std::unordered_map<uint32_t, IDirect3DBaseTexture9 *> m_d3d9_replacements;
        std::unordered_map<uint32_t, ID3D11ShaderResourceView *> m_d3d11_replacements;

        void release_replacements();

        // Background dump worker
        struct DumpRequest
        {
            uint32_t hash = 0;
            UINT width = 0;
            UINT height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::vector<uint8_t> data;
            UINT row_pitch = 0;
        };

        std::vector<DumpRequest> m_dump_queue;
        std::thread m_dump_thread;
        std::condition_variable m_dump_cv;
        std::mutex m_dump_mutex;
        bool m_dump_thread_running = false;

        void dump_worker_loop();

        std::filesystem::path find_injection_path(uint32_t hash);
    };
}
