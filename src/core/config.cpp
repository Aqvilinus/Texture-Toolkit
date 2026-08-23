#include "core/config.h"

#include "core/logger.h"
#include <fstream>
#include <sstream>
#include <imgui.h>

// Declared non-static by the Win32 backend specifically so callers outside it can use it.
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{
    std::string hotkey_name(uint32_t vk)
    {
        // ImGui's names rather than GetKeyNameTextW, which needs the extended-key bit set right
        // (Insert comes back as "Num 0"), returns nothing for Pause, and localises into glyphs the
        // Latin-only ImGui font cannot draw.
        //
        // The scancode in lParam is not optional: punctuation is mapped by scancode, its virtual
        // key differing per layout (tilde is VK_OEM_3 on EN-US, VK_OEM_8 on EN-UK, VK_OEM_7 on FR).
        const LPARAM lparam = static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
        const ImGuiKey key = ImGui_ImplWin32_KeyEventToImGuiKey(static_cast<WPARAM>(vk), lparam);

        return key != ImGuiKey_None ? ImGui::GetKeyName(key) : Logger::fmt("key 0x%02X", vk);
    }

    ConfigManager &ConfigManager::get()
    {
        static ConfigManager instance;
        return instance;
    }

    void ConfigManager::init(const std::filesystem::path &config_dir)
    {
        m_ini_path = config_dir / "TextureToolkit.ini";
        load();
    }

    void ConfigManager::load()
    {
        if (!std::filesystem::exists(m_ini_path))
        {
            save(); // Auto-generate default INI file
            return;
        }

        // A std::wstring, not a MAX_PATH buffer: wcscpy_s aborts the process on overflow, and a
        // game under a deep mod folder can exceed it.
        const std::wstring ini_path = m_ini_path.wstring();
        const wchar_t *ini_w = ini_path.c_str();

        wchar_t hotkey_str[32] = L"";
        GetPrivateProfileStringW(L"TextureToolkit", L"HotKey", L"0x2D", hotkey_str, 32, ini_w);
        try
        {
            std::wstring hs(hotkey_str);
            m_config.hotkey = static_cast<uint32_t>(std::stoul(hs, nullptr, 16));
        }
        catch (...)
        {
            m_config.hotkey = 0;
        }

        // A parse that succeeds is not a key that works: HotKey=0x0 or anything past the highest
        // virtual-key code leaves the panel with no way to open and nothing in the log to say why.
        if (m_config.hotkey == 0 || m_config.hotkey > 0xFE)
        {
            Logger::get().warn("[Config] HotKey=" + std::filesystem::path(hotkey_str).string() +
                               " is not a virtual-key code; using INSERT.");
            m_config.hotkey = VK_INSERT;
        }

        wchar_t root_str[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"TextureToolkit", L"ResourceRoot", L"TT", root_str, MAX_PATH, ini_w);
        m_config.resource_root = root_str;

        // Empty would put dump/ and inject/ straight into the game folder.
        if (m_config.resource_root.empty())
        {
            Logger::get().warn("[Config] ResourceRoot is empty; using TT.");
            m_config.resource_root = L"TT";
        }

        m_config.enable_injection = GetPrivateProfileIntW(L"TextureToolkit", L"EnableInjection", 1, ini_w) != 0;
        m_config.auto_dump = GetPrivateProfileIntW(L"TextureToolkit", L"AutoDump", 0, ini_w) != 0;
        m_config.filter_small_textures = GetPrivateProfileIntW(L"TextureToolkit", L"FilterSmallTextures", 1, ini_w) != 0;
        m_config.show_current_frame_only = GetPrivateProfileIntW(L"TextureToolkit", L"ShowCurrentFrameOnly", 1, ini_w) != 0;
        m_config.track_map_unmap = GetPrivateProfileIntW(L"TextureToolkit", L"TrackMapUnmap", 1, ini_w) != 0;
        m_config.enable_overlay = GetPrivateProfileIntW(L"TextureToolkit", L"EnableOverlay", 1, ini_w) != 0;

        m_config.show_osd_banner = GetPrivateProfileIntW(L"TextureToolkit", L"ShowOSDBanner", 1, ini_w) != 0;

        m_config.verbose = GetPrivateProfileIntW(L"TextureToolkit", L"Verbose", 0, ini_w) != 0;

        Logger::get().info("[ConfigManager] Configuration loaded from " + m_ini_path.string());
    }

    void ConfigManager::save()
    {
        std::error_code ec;
        std::filesystem::create_directories(m_ini_path.parent_path(), ec);

        if (!std::filesystem::exists(m_ini_path, ec))
            write_template();

        // Key by key rather than rewriting the file: this runs on every toggle in the panel, and a
        // rewrite would drop whatever the user put in the file by hand.
        const std::wstring ini = m_ini_path.wstring();
        auto put = [&ini](const wchar_t *key, const std::wstring &value) {
            WritePrivateProfileStringW(L"TextureToolkit", key, value.c_str(), ini.c_str());
        };
        auto put_flag = [&put](const wchar_t *key, bool value) {
            put(key, value ? L"1" : L"0");
        };

        wchar_t hotkey[16] = {};
        swprintf_s(hotkey, L"0x%X", m_config.hotkey);
        put(L"HotKey", hotkey);
        put(L"ResourceRoot", m_config.resource_root.wstring());

        put_flag(L"EnableInjection", m_config.enable_injection);
        put_flag(L"AutoDump", m_config.auto_dump);
        put_flag(L"FilterSmallTextures", m_config.filter_small_textures);
        put_flag(L"ShowCurrentFrameOnly", m_config.show_current_frame_only);
        put_flag(L"ShowOSDBanner", m_config.show_osd_banner);
        put_flag(L"TrackMapUnmap", m_config.track_map_unmap);
        put_flag(L"EnableOverlay", m_config.enable_overlay);
        put_flag(L"Verbose", m_config.verbose);

        // The profile API caches writes; this flushes them to disk.
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    }

    // Only ever written once, when there is no file: the comments explain what the names cannot,
    // and a key added in a later version will not appear in a file that already exists.
    void ConfigManager::write_template()
    {
        std::ofstream file(m_ini_path, std::ios::out | std::ios::trunc);
        if (!file.is_open())
            return;

        file << "[TextureToolkit]\n"
             << "; 0x2D = INSERT, 0x24 = HOME, 0x74 = F5, 0x79 = F10\n"
             << "HotKey=0x" << std::hex << std::uppercase << m_config.hotkey << "\n\n"
             << "; Holds dump/, inject/ and imgui.ini. Relative to the game folder, or absolute.\n"
             << "ResourceRoot=" << m_config.resource_root.string() << "\n\n"
             << "EnableInjection=" << (m_config.enable_injection ? 1 : 0) << "\n"
             << "AutoDump=" << (m_config.auto_dump ? 1 : 0) << "\n"
             << "FilterSmallTextures=" << (m_config.filter_small_textures ? 1 : 0) << "\n"
             << "ShowCurrentFrameOnly=" << (m_config.show_current_frame_only ? 1 : 0) << "\n"
             << "ShowOSDBanner=" << (m_config.show_osd_banner ? 1 : 0) << "\n\n"
             << "; Costs a CRC32 over the whole surface on every Unmap. Only for games that\n"
             << "; upload textures that way rather than through CreateTexture2D.\n"
             << "TrackMapUnmap=" << (m_config.track_map_unmap ? 1 : 0) << "\n\n"
             << "; Off leaves texture replacement running with no panel and no input hooks.\n"
             << "EnableOverlay=" << (m_config.enable_overlay ? 1 : 0) << "\n\n"
             << "; Per-texture logging, slow.\n"
             << "Verbose=" << (m_config.verbose ? 1 : 0) << "\n";

        Logger::get().info("[ConfigManager] Created " + m_ini_path.string());
    }
}
