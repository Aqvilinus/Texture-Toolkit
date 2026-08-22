#pragma once

// Performance instrumentation: fps every 3 s, per-hook milliseconds, hashed megabytes, and a
// line for every frame slower than 20 ms. Off in normal use. Error reporting is separate and
// always on. Read the slow-frame lines, not the averages -- see kSlowFrameMs.
#define TT_DIAGNOSTICS 0

#include <string>
#include <filesystem>
#include <windows.h>

namespace TextureToolkit
{
    struct Configuration
    {
        uint32_t hotkey = VK_INSERT; // Default: INSERT key (0x2D)

        // Root folder for all of Texture Toolkit's runtime files (dump/, inject/, imgui.ini).
        // Relative to the game's executable folder, or an absolute path. Rename or relocate
        // this one value and everything moves together.
        std::filesystem::path resource_root = "TT";

        bool enable_injection = true;
        bool auto_dump = false;
        bool filter_small_textures = true;
        bool show_current_frame_only = true;

        // Track textures the game fills through Map/Unmap rather than handing the pixels to
        // CreateTexture2D. Needed by games that create empty textures and write into them,
        // useless otherwise, and costly: see the note where the hooks are installed.
        bool track_map_unmap = true;

        // The panel and everything serving it: the ImGui overlay and the process-wide input
        // detours (PeekMessage, GetMessage, GetKeyState, ClipCursor, the window procedure) that
        // let it take input from the game. Off = a texture replacer and nothing else.
        bool enable_overlay = true;

        bool show_osd_banner = true;
        float osd_duration_seconds = 6.0f;

        // When true, per-texture/per-hook Debug logging is written (very chatty).
        bool verbose = false;
    };

    // Printable name of the configured toggle key, for every place that tells the user which
    // key opens the panel.
    std::string hotkey_name(uint32_t vk);

    class ConfigManager
    {
    public:
        static ConfigManager &get();

        void init(const std::filesystem::path &config_dir);
        void load();
        void save();
        void write_template();

        Configuration &get_config() { return m_config; }

    private:
        ConfigManager() = default;

        std::filesystem::path m_ini_path;
        Configuration m_config;
    };
}
