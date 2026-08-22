#include "input/input_hook.h"

#include <utility>
#include "core/hook_manager.h"
#include "core/iat_hook.h"
#include "ui/overlay.h"
#include "core/config.h"
#include "core/logger.h"
#include <intrin.h>
#include "imgui.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern HMODULE g_our_module;

namespace TextureToolkit
{
    // Thread-local, so only the thread that is actually inside the ImGui backend is exempt.
    static thread_local bool s_input_passthrough = false;

    InputHook &InputHook::get()
    {
        static InputHook instance;
        return instance;
    }

    InputHook::~InputHook()
    {
        shutdown();
    }

    bool InputHook::init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;

        HMODULE dinput8_module = GetModuleHandleA("dinput8.dll");
        if (dinput8_module == nullptr)
        {
            dinput8_module = LoadLibraryA("dinput8.dll");
        }

        if (dinput8_module != nullptr)
        {
            void *pDirectInput8Create = reinterpret_cast<void *>(GetProcAddress(dinput8_module, "DirectInput8Create"));
            if (pDirectInput8Create != nullptr)
            {
                HookManager::get().create_hook(pDirectInput8Create, &Hooked_DirectInput8Create, reinterpret_cast<void **>(&m_orig_dinput8_create));
                IATHook::hook_all_modules("dinput8.dll", "DirectInput8Create", &Hooked_DirectInput8Create, reinterpret_cast<void **>(&m_orig_dinput8_create));
                Logger::get().info("[InputHook] DirectInput8Create API & IAT hooks installed successfully.");
            }
        }

        HMODULE user32_module = GetModuleHandleA("user32.dll");
        if (user32_module != nullptr)
        {
            void *pSetCursorPos = reinterpret_cast<void *>(GetProcAddress(user32_module, "SetCursorPos"));
            if (pSetCursorPos != nullptr)
            {
                HookManager::get().create_hook(pSetCursorPos, &Hooked_SetCursorPos, reinterpret_cast<void **>(&m_orig_set_cursor_pos));
            }

            void *pClipCursor = reinterpret_cast<void *>(GetProcAddress(user32_module, "ClipCursor"));
            if (pClipCursor != nullptr)
            {
                HookManager::get().create_hook(pClipCursor, &Hooked_ClipCursor, reinterpret_cast<void **>(&m_orig_clip_cursor));
            }

            void *pPeekMessageA = reinterpret_cast<void *>(GetProcAddress(user32_module, "PeekMessageA"));
            if (pPeekMessageA != nullptr)
            {
                HookManager::get().create_hook(pPeekMessageA, &Hooked_PeekMessageA, reinterpret_cast<void **>(&m_orig_peek_message_a));
            }

            void *pPeekMessageW = reinterpret_cast<void *>(GetProcAddress(user32_module, "PeekMessageW"));
            if (pPeekMessageW != nullptr)
            {
                HookManager::get().create_hook(pPeekMessageW, &Hooked_PeekMessageW, reinterpret_cast<void **>(&m_orig_peek_message_w));
            }

            void *pGetMessageA = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetMessageA"));
            if (pGetMessageA != nullptr)
            {
                HookManager::get().create_hook(pGetMessageA, &Hooked_GetMessageA, reinterpret_cast<void **>(&m_orig_get_message_a));
            }

            void *pGetMessageW = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetMessageW"));
            if (pGetMessageW != nullptr)
            {
                HookManager::get().create_hook(pGetMessageW, &Hooked_GetMessageW, reinterpret_cast<void **>(&m_orig_get_message_w));
            }

            void *pGetAsyncKeyState = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetAsyncKeyState"));
            if (pGetAsyncKeyState != nullptr)
            {
                HookManager::get().create_hook(pGetAsyncKeyState, &Hooked_GetAsyncKeyState, reinterpret_cast<void **>(&m_orig_get_async_key_state));
            }

            void *pGetKeyState = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetKeyState"));
            if (pGetKeyState != nullptr)
            {
                HookManager::get().create_hook(pGetKeyState, &Hooked_GetKeyState, reinterpret_cast<void **>(&m_orig_get_key_state));
            }

            void *pGetKeyboardState = reinterpret_cast<void *>(GetProcAddress(user32_module, "GetKeyboardState"));
            if (pGetKeyboardState != nullptr)
            {
                HookManager::get().create_hook(pGetKeyboardState, &Hooked_GetKeyboardState, reinterpret_cast<void **>(&m_orig_get_keyboard_state));
            }

            Logger::get().info("[InputHook] User32 cursor, input and message hooks installed successfully.");
        }

        m_initialized = true;
        return true;
    }

    void InputHook::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_initialized = false;
    }

    // --- Reading input on our own behalf ---

    bool InputHook::PassthroughScope::active() { return s_input_passthrough; }

    InputHook::PassthroughScope::PassthroughScope()
        : m_previous(std::exchange(s_input_passthrough, true))
    {
    }

    InputHook::PassthroughScope::~PassthroughScope()
    {
        s_input_passthrough = m_previous;
    }

    SHORT InputHook::real_async_key_state(int vk)
    {
        return get().m_orig_get_async_key_state ? get().m_orig_get_async_key_state(vk) : ::GetAsyncKeyState(vk);
    }

    SHORT InputHook::real_key_state(int vk)
    {
        return get().m_orig_get_key_state ? get().m_orig_get_key_state(vk) : ::GetKeyState(vk);
    }

    // The one question every hook here asks. Special K keeps the same decision in a single
    // predicate; repeating the two conditions per hook is how they drift apart.
    bool InputHook::overlay_owns_input()
    {
        return TextureToolkitUI::is_visible() && !PassthroughScope::active();
    }

    // --- DirectInput 8 ---

    void InputHook::hook_interface(IDirectInput8 *dinput)
    {
        if (dinput == nullptr) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hooked_interfaces.count(dinput)) return;

        void **vtable = *reinterpret_cast<void ***>(dinput);
        void *create_device_addr = vtable[3]; // IDirectInput8::CreateDevice is index 3

        if (m_orig_create_device == nullptr)
        {
            HookManager::get().create_hook(create_device_addr, &Hooked_CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
            Logger::get().info("[InputHook] Intercepted IDirectInput8::CreateDevice.");
        }

        m_hooked_interfaces.insert(dinput);
    }

    void InputHook::hook_device(IDirectInputDevice8 *device)
    {
        if (device == nullptr) return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hooked_devices.count(device)) return;

        void **vtable = *reinterpret_cast<void ***>(device);
        void *get_state_addr = vtable[9]; // IDirectInputDevice8::GetDeviceState is index 9
        void *get_data_addr = vtable[10]; // IDirectInputDevice8::GetDeviceData is index 10

        if (m_orig_get_device_state == nullptr)
        {
            HookManager::get().create_hook(get_state_addr, &Hooked_GetDeviceState, reinterpret_cast<void **>(&m_orig_get_device_state));
            HookManager::get().create_hook(get_data_addr, &Hooked_GetDeviceData, reinterpret_cast<void **>(&m_orig_get_device_data));
            Logger::get().info("[InputHook] REAL GAME DINPUT8 DEVICE INTERCEPTED! GetDeviceState/GetDeviceData hooked.");
        }

        m_hooked_devices.insert(device);
    }

    HRESULT WINAPI InputHook::Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter)
    {
        HRESULT hr = E_FAIL;
        if (get().m_orig_dinput8_create)
        {
            hr = get().m_orig_dinput8_create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
        }

        if (SUCCEEDED(hr) && ppvOut != nullptr && *ppvOut != nullptr)
        {
            get().hook_interface(static_cast<IDirectInput8 *>(*ppvOut));
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE InputHook::Hooked_CreateDevice(IDirectInput8 *pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
    {
        HRESULT hr = E_FAIL;
        if (get().m_orig_create_device)
        {
            hr = get().m_orig_create_device(pThis, rguid, lplpDirectInputDevice, pUnkOuter);
        }

        if (SUCCEEDED(hr) && lplpDirectInputDevice != nullptr && *lplpDirectInputDevice != nullptr)
        {
            get().hook_device(*lplpDirectInputDevice);
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE InputHook::Hooked_GetDeviceState(IDirectInputDevice8 *pThis, DWORD cbData, LPVOID lpvData)
    {
        HRESULT hr = get().m_orig_get_device_state(pThis, cbData, lpvData);

        if (SUCCEEDED(hr) && TextureToolkitUI::is_visible())
        {
            // Block input to the game by clearing the buffer!
            if (lpvData != nullptr && cbData > 0)
            {
                memset(lpvData, 0, cbData);
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE InputHook::Hooked_GetDeviceData(IDirectInputDevice8 *pThis, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags)
    {
        HRESULT hr = get().m_orig_get_device_data(pThis, cbObjectData, rgdod, pdwInOut, dwFlags);

        if (SUCCEEDED(hr) && TextureToolkitUI::is_visible())
        {
            // Block input by simulating zero events read
            if (pdwInOut != nullptr)
            {
                *pdwInOut = 0;
            }
        }

        return hr;
    }

    // --- The window message queue ---

    bool InputHook::feed_to_overlay(LPMSG lpMsg)
    {
        if (lpMsg == nullptr) return false;

        if (lpMsg->message == WM_INPUT)
            return true;

        if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST) ||
            (lpMsg->message >= WM_MOUSEFIRST && lpMsg->message <= WM_MOUSELAST))
        {
            PassthroughScope pass;
            ImGui_ImplWin32_WndProcHandler(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
            return true; // Block input message from reaching the game
        }

        return false;
    }

    // Four message-pump entry points differ only in which original they call.
    BOOL InputHook::swallow_if_ours(BOOL got_message, LPMSG msg)
    {
        if (got_message && msg != nullptr && TextureToolkitUI::is_visible() && feed_to_overlay(msg))
            msg->message = WM_NULL;
        return got_message;
    }

    BOOL WINAPI InputHook::Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        return InputHook::swallow_if_ours(get().m_orig_peek_message_a(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg), lpMsg);
    }

    BOOL WINAPI InputHook::Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        return InputHook::swallow_if_ours(get().m_orig_peek_message_w(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg), lpMsg);
    }

    BOOL WINAPI InputHook::Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
    {
        return InputHook::swallow_if_ours(get().m_orig_get_message_a(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax), lpMsg);
    }

    BOOL WINAPI InputHook::Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
    {
        return InputHook::swallow_if_ours(get().m_orig_get_message_w(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax), lpMsg);
    }

    // --- Polled keyboard state ---

    SHORT WINAPI InputHook::Hooked_GetAsyncKeyState(int vKey)
    {
        if (overlay_owns_input())
        {
            if (vKey != static_cast<int>(ConfigManager::get().get_config().hotkey))
            {
                return 0;
            }
        }
        return get().m_orig_get_async_key_state(vKey);
    }

    SHORT WINAPI InputHook::Hooked_GetKeyState(int vKey)
    {
        if (overlay_owns_input())
        {
            if (vKey != static_cast<int>(ConfigManager::get().get_config().hotkey))
            {
                return 0;
            }
        }
        return get().m_orig_get_key_state(vKey);
    }

    BOOL WINAPI InputHook::Hooked_GetKeyboardState(PBYTE lpKeyState)
    {
        BOOL ret = get().m_orig_get_keyboard_state(lpKeyState);
        if (ret && overlay_owns_input())
        {
            int toggle_key = static_cast<int>(ConfigManager::get().get_config().hotkey);
            BYTE toggle_state = lpKeyState[toggle_key];
            memset(lpKeyState, 0, 256);
            lpKeyState[toggle_key] = toggle_state;
        }
        return ret;
    }

    // --- The cursor ---

    BOOL WINAPI InputHook::Hooked_SetCursorPos(int X, int Y)
    {
        if (TextureToolkitUI::is_visible())
        {
            return TRUE; // Ignore and report success (blocks game cursor centering)
        }
        return get().m_orig_set_cursor_pos(X, Y);
    }

    BOOL WINAPI InputHook::Hooked_ClipCursor(const RECT *lpRect)
    {
        if (TextureToolkitUI::is_visible())
        {
            return TRUE; // Block game cursor clipping
        }
        return get().m_orig_clip_cursor(lpRect);
    }
}
