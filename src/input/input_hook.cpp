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
            // A row per export, the way the D3DX entry points are installed on the D3D9 side.
            // Written out one by one, this was eleven copies of the same five lines -- which is
            // where the wrong pointer ends up beside the wrong name without anyone noticing.
            const struct Entry
            {
                const char *name;
                void *detour;
                void **original;
            } entries[] = {
                { "SetCursorPos", &Hooked_SetCursorPos, reinterpret_cast<void **>(&m_orig_set_cursor_pos) },
                { "ClipCursor", &Hooked_ClipCursor, reinterpret_cast<void **>(&m_orig_clip_cursor) },
                { "PeekMessageA", &Hooked_PeekMessageA, reinterpret_cast<void **>(&m_orig_peek_message_a) },
                { "PeekMessageW", &Hooked_PeekMessageW, reinterpret_cast<void **>(&m_orig_peek_message_w) },
                { "GetMessageA", &Hooked_GetMessageA, reinterpret_cast<void **>(&m_orig_get_message_a) },
                { "GetMessageW", &Hooked_GetMessageW, reinterpret_cast<void **>(&m_orig_get_message_w) },
                { "DispatchMessageA", &Hooked_DispatchMessageA, reinterpret_cast<void **>(&m_orig_dispatch_message_a) },
                { "DispatchMessageW", &Hooked_DispatchMessageW, reinterpret_cast<void **>(&m_orig_dispatch_message_w) },
                { "GetAsyncKeyState", &Hooked_GetAsyncKeyState, reinterpret_cast<void **>(&m_orig_get_async_key_state) },
                { "GetKeyState", &Hooked_GetKeyState, reinterpret_cast<void **>(&m_orig_get_key_state) },
                { "GetKeyboardState", &Hooked_GetKeyboardState, reinterpret_cast<void **>(&m_orig_get_keyboard_state) },
            };

            for (const Entry &entry : entries)
            {
                if (void *address = reinterpret_cast<void *>(GetProcAddress(user32_module, entry.name)))
                    HookManager::get().create_hook(address, entry.detour, entry.original);
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

    bool InputHook::overlay_consumed(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT &result)
    {
        if (!TextureToolkitUI::is_visible())
            return false;

        LRESULT from_backend = 0;
        {
            PassthroughScope pass;
            from_backend = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        }

        // The backend answers non-zero to mean it set the cursor itself. Dropping that answer let
        // the game re-assert its own on every pointer move.
        if (message == WM_SETCURSOR && from_backend != 0)
        {
            result = from_backend;
            return true;
        }

        // Sent, never posted, so it arrives only from the menu loop inside user32 -- and left to
        // the game it is what makes Windows beep at a key pressed with Alt held.
        if (message == WM_MENUCHAR)
        {
            result = MAKELRESULT(0, MNC_CLOSE);
            return true;
        }

        if (message == WM_INPUT ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST))
        {
            result = 0;
            return true;
        }

        return false;
    }

    // Still needed although dispatch is intercepted below: user32 runs modal loops of its own for
    // menus and for moving and sizing a window, and those pump messages without calling the
    // exported DispatchMessage. Nor does a message that is sent rather than posted pass through it.
    LRESULT CALLBACK InputHook::Hooked_WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        LRESULT result = 0;
        if (overlay_consumed(window, message, wparam, lparam, result))
            return result;

        return CallWindowProc(get().m_orig_wndproc, window, message, wparam, lparam);
    }

    void InputHook::attach_to_window(HWND window)
    {
        if (window == nullptr || m_orig_wndproc != nullptr)
            return;

        m_window = window;
        m_orig_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hooked_WndProc)));
    }

    void InputHook::detach_from_window()
    {
        if (m_window == nullptr || m_orig_wndproc == nullptr)
            return;

        SetWindowLongPtr(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_orig_wndproc));
        m_orig_wndproc = nullptr;
        m_window = nullptr;
    }

    BOOL InputHook::swallow_raw_input(BOOL got_message, LPMSG msg)
    {
        if (got_message && msg != nullptr && msg->message == WM_INPUT && TextureToolkitUI::is_visible())
            msg->message = WM_NULL;
        return got_message;
    }

    BOOL WINAPI InputHook::Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        return InputHook::swallow_raw_input(get().m_orig_peek_message_a(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg), lpMsg);
    }

    BOOL WINAPI InputHook::Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        return InputHook::swallow_raw_input(get().m_orig_peek_message_w(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg), lpMsg);
    }

    BOOL WINAPI InputHook::Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
    {
        return InputHook::swallow_raw_input(get().m_orig_get_message_a(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax), lpMsg);
    }

    BOOL WINAPI InputHook::Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
    {
        return InputHook::swallow_raw_input(get().m_orig_get_message_w(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax), lpMsg);
    }

    // The game's own window procedure is skipped, not the message: default processing still runs,
    // so the window keeps behaving like a window. This is where Special K takes input away too.
    LRESULT WINAPI InputHook::Hooked_DispatchMessageA(const MSG *lpMsg)
    {
        LRESULT ignored = 0;
        if (lpMsg != nullptr && overlay_consumed(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam, ignored))
            return DefWindowProcA(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);

        return get().m_orig_dispatch_message_a(lpMsg);
    }

    LRESULT WINAPI InputHook::Hooked_DispatchMessageW(const MSG *lpMsg)
    {
        LRESULT ignored = 0;
        if (lpMsg != nullptr && overlay_consumed(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam, ignored))
            return DefWindowProcW(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);

        return get().m_orig_dispatch_message_w(lpMsg);
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
