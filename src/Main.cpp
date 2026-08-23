/*
 * Texture Toolkit Standalone -- originally by BadassBaboon
 * Native Proxy Wrapper & ASI Plugin for Direct3D 9 & Direct3D 11
 */

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include "core/config.h"
#include "core/version.h"
#include "core/logger.h"
#include "core/hook_manager.h"
#include "texture/texture_manager.h"
#include "render/d3d11/d3d11_texture_manager.h"
#include "render/d3d9/d3d9_texture_manager.h"
#include "render/d3d9/d3d9_hook.h"
#include "render/d3d11/d3d11_hook.h"
#include "render/dxgi/dxgi_hook.h"
#include "input/input_hook.h"

// Handle to our own module (.asi). Set in DllMain before initialize_standalone runs.
HMODULE g_our_module = nullptr;

namespace TextureToolkit
{
    void initialize_standalone()
    {
        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));
        std::filesystem::path game_dir = std::filesystem::path(exe_path).parent_path();

        // Keep the .ini and .log next to the .asi (usually the plugins/ or scripts/ folder),
        // so they are easy to find and the dump/inject folders can be re-pointed in the .ini.
        wchar_t module_path[MAX_PATH] = L"";
        GetModuleFileNameW(g_our_module, module_path, ARRAYSIZE(module_path));
        std::filesystem::path asi_dir = std::filesystem::path(module_path).parent_path();
        if (asi_dir.empty())
            asi_dir = game_dir;

        // All of this runs under the loader lock: files, the INI, MinHook and the detours. That is
        // a knowingly accepted risk, not an absence of one -- hooks have to be in place before the
        // game's first D3D call, and an ASI has no earlier entry point. Only device creation is
        // deferred to a thread, because it deadlocks here.
        Logger::get().init(asi_dir);
        Logger::get().info("[Main] Texture Toolkit v" TT_VERSION " (" TT_ARCH ", built " __DATE__ " " __TIME__ ") initializing...");
        Logger::get().info("[Main] Host: " + std::filesystem::path(exe_path).string());
        Logger::get().info("[Main] Loaded from: " + std::filesystem::path(module_path).string());

        ConfigManager::get().init(asi_dir);
        Logger::get().set_min_level(ConfigManager::get().get_config().verbose ? LogLevel::Debug : LogLevel::Info);

        D3D11TextureManager::get().init();
        D3D9TextureManager::get().init();
        HookManager::get().init();

        // These exist only so the panel can take input from the game; with the panel off they
        // are pure overhead on paths walked thousands of times a second.
        if (ConfigManager::get().get_config().enable_overlay)
            InputHook::get().init();
        D3D9Hook::get().init();
        D3D11Hook::get().init();
        DXGIHook::get().init();

        const auto &cfg = ConfigManager::get().get_config();
        Logger::get().info(cfg.enable_overlay
                               ? "[Main] Initialization complete. Press " + hotkey_name(cfg.hotkey) + " to open the Texture Toolkit panel."
                               : std::string("[Main] Initialization complete. Panel disabled by config (EnableOverlay=0); texture replacement only."));
    }

    void shutdown_standalone()
    {
        Logger::get().info("[Main] Texture Toolkit Standalone shutting down...");

        // Detours first: while any of them is live it can still call into a manager, and a manager
        // that has already released its device objects would be reached through a stale pointer.
        DXGIHook::get().begin_shutdown();
        InputHook::get().shutdown();
        D3D9Hook::get().shutdown();
        D3D11Hook::get().shutdown();
        HookManager::get().shutdown();

        D3D11TextureManager::get().shutdown();
        D3D9TextureManager::get().shutdown();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_our_module = hModule;
        DisableThreadLibraryCalls(hModule);
        // We MUST initialize synchronously so that our hooks are installed
        // BEFORE the game continues its execution and calls D3D initialization functions!
        TextureToolkit::initialize_standalone();
        break;

    case DLL_PROCESS_DETACH:
        // lpvReserved != NULL means the PROCESS is terminating. Per MSDN, we must NOT do
        // cleanup here: the OS is already tearing the process down and every other thread
        // has been terminated, so joining our worker thread or calling COM Release()/D3D
        // teardown under the loader lock deadlocks (observed as a black-screen hang when
        // quitting Bully). The OS reclaims all of it anyway. Only clean up on an explicit
        // FreeLibrary unload (lpvReserved == NULL), which is the safe, orderly case.
        if (lpvReserved == nullptr)
            TextureToolkit::shutdown_standalone();
        break;
    }

    return TRUE;
}
