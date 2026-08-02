#pragma once

#include <windows.h>
#include <MinHook.h>
#include <string>
#include <mutex>
#include <unordered_map>
#include "Logger.h"

namespace TextureToolkit
{
    class HookManager
    {
    public:
        static HookManager &get();

        bool init();
        void shutdown();

        template <typename T>
        bool create_hook(void *target, void *detour, T **original)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // init() must have been called first (Main.cpp guarantees this). We must not
            // call it here: it locks m_mutex, which we already hold, and the mutex is
            // non-recursive -> deadlock. If MinHook is not initialized MH_CreateHook
            // simply returns an error, handled below.
            MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void **>(original));
            if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
            {
                Logger::get().error("[HookManager] Failed to create hook at address 0x" + 
                    std::to_string(reinterpret_cast<uintptr_t>(target)) + ", MH_STATUS: " + std::to_string(status));
                return false;
            }

            status = MH_EnableHook(target);
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                Logger::get().error("[HookManager] Failed to enable hook at address 0x" + 
                    std::to_string(reinterpret_cast<uintptr_t>(target)) + ", MH_STATUS: " + std::to_string(status));
                return false;
            }

            m_active_hooks[target] = reinterpret_cast<void *>(original);
            return true;
        }

    private:
        HookManager() = default;
        ~HookManager();

        std::mutex m_mutex;
        bool m_initialized = false;
        std::unordered_map<void *, void *> m_active_hooks;
    };
}
