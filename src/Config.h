#pragma once

#include <string>
#include <filesystem>
#include <windows.h>

namespace TextureToolkit
{
    struct Configuration
    {
        uint32_t hotkey = VK_INSERT; // Default: INSERT key (0x2D)
        std::filesystem::path dump_dir = "TT/dump";
        std::filesystem::path inject_dir = "TT/inject";

        bool enable_injection = true;
        bool auto_dump = false;
        bool filter_small_textures = true;
        bool show_current_frame_only = false;

        bool show_osd_banner = true;
        float osd_duration_seconds = 6.0f;

        // When true, per-texture/per-hook Debug logging is written (very chatty).
        bool verbose = false;
    };

    class ConfigManager
    {
    public:
        static ConfigManager &get();

        void init(const std::filesystem::path &game_dir);
        void load();
        void save();

        Configuration &get_config() { return m_config; }

    private:
        ConfigManager() = default;

        std::filesystem::path m_ini_path;
        Configuration m_config;
    };
}
