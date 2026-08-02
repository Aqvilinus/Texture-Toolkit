/*
 * Texture Toolkit Standalone v1.0.0 by BadassBaboon
 * Native Proxy Wrapper & ASI Plugin for Direct3D 9 & Direct3D 11
 */

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include "Config.h"
#include "Logger.h"
#include "HookManager.h"
#include "TextureManager.h"
#include "D3D9Hook.h"
#include "D3D11Hook.h"
#include "DInput8Hook.h"

namespace TextureToolkit
{
    void initialize_standalone()
    {
        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));
        std::filesystem::path game_dir = std::filesystem::path(exe_path).parent_path();
        std::filesystem::path tt_dir = game_dir / "TT";

        Logger::get().init(tt_dir);
        Logger::get().info("[Main] Texture Toolkit Standalone v1.0.0 initializing...");

        ConfigManager::get().init(game_dir);
        Logger::get().set_min_level(ConfigManager::get().get_config().verbose ? LogLevel::Debug : LogLevel::Info);

        TextureManager::get().init();
        HookManager::get().init();

        DInput8Hook::get().init();
        D3D9Hook::get().init();
        D3D11Hook::get().init();

        Logger::get().info("[Main] Initialization complete. Press INSERT to open Texture Toolkit UI.");
    }

    void shutdown_standalone()
    {
        Logger::get().info("[Main] Texture Toolkit Standalone shutting down...");
        TextureManager::get().shutdown();
        DInput8Hook::get().shutdown();
        D3D9Hook::get().shutdown();
        D3D11Hook::get().shutdown();
        HookManager::get().shutdown();
    }
}

HMODULE g_our_module = nullptr;

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
