#pragma once

#include <windows.h>
#include <dinput.h>
#include <mutex>
#include <unordered_set>

namespace TextureToolkit
{
    // Stands between the game and the mouse and keyboard while the panel is up. Two of the ways a
    // game can read input are covered -- DirectInput 8 devices and the window message queue -- so
    // a title reading Raw Input, XInput or HID directly keeps moving while the panel has focus.
    // Special K hooks all of those; each is its own subsystem there, and would be here too.
    class InputHook
    {
    public:
        static InputHook &get();
        ~InputHook();

        bool init();
        void shutdown();

        // DirectInput hands out interfaces one at a time, so each is hooked as it appears rather
        // than at startup.
        void hook_interface(IDirectInput8 *dinput);
        void hook_device(IDirectInputDevice8 *device);

        // The true key state, bypassing our own mask. Anything in Texture Toolkit that polls input
        // must use these, or it reads back the zeros the mask feeds the game.
        static SHORT real_async_key_state(int vk);
        static SHORT real_key_state(int vk);

        // Exempts the current thread from masking for the duration of a scope -- the ImGui backend
        // reads modifier state with GetKeyState itself. Thread-local, so a game polling on another
        // thread stays masked; scoped, so an early return cannot leave it set.
        class PassthroughScope
        {
        public:
            PassthroughScope();
            ~PassthroughScope();

            static bool active();

        private:
            bool m_previous;
        };

    private:
        InputHook() = default;

        // The one question every detour below asks.
        static bool overlay_owns_input();

        // --- DirectInput 8: the entry point, then the two ways a device is read ---

        static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(IDirectInput8 *pThis, REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDirectInputDevice, LPUNKNOWN pUnkOuter);
        static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceState(IDirectInputDevice8 *pThis, DWORD cbData, LPVOID lpvData);
        static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceData(IDirectInputDevice8 *pThis, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags);

        // Signatures are written out rather than derived, so what the trampoline expects is visible
        // here. They must match the Windows headers exactly: create_hook passes the trampoline
        // through void **, so a mismatch compiles and only shows up as a corrupted stack.
        using DirectInput8Create_fn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
        using CreateDevice_fn = HRESULT(STDMETHODCALLTYPE *)(IDirectInput8 *, REFGUID, LPDIRECTINPUTDEVICE8 *, LPUNKNOWN);
        using GetDeviceState_fn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8 *, DWORD, LPVOID);
        using GetDeviceData_fn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8 *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

        DirectInput8Create_fn m_orig_dinput8_create = nullptr;
        CreateDevice_fn m_orig_create_device = nullptr;
        GetDeviceState_fn m_orig_get_device_state = nullptr;
        GetDeviceData_fn m_orig_get_device_data = nullptr;

        // A game asks for the same interface repeatedly; each vtable must be patched once.
        std::unordered_set<IDirectInput8 *> m_hooked_interfaces;
        std::unordered_set<IDirectInputDevice8 *> m_hooked_devices;

        // --- The window message queue ---
        //
        // Messages are taken away from the game at dispatch, not in the queue, which is where
        // Special K takes them. It has to be that way round: a key press only becomes a character
        // when the game's own pump runs TranslateMessage on it, so a message blanked in the queue
        // is a character never born, and a text field in the panel can never receive one. Left
        // alone until dispatch, the character appears by itself -- and in the window's encoding,
        // which is the one the ImGui backend assumes.
        //
        // Only raw input is still taken in the queue: nothing translates it, so there is nothing
        // to wait for.

        static BOOL WINAPI Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
        static BOOL WINAPI Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
        static BOOL WINAPI Hooked_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
        static BOOL WINAPI Hooked_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
        static LRESULT WINAPI Hooked_DispatchMessageA(const MSG *lpMsg);
        static LRESULT WINAPI Hooked_DispatchMessageW(const MSG *lpMsg);

        static BOOL swallow_raw_input(BOOL got_message, LPMSG msg);

        // Feeds the message to the overlay and says whether the game's window procedure should be
        // skipped for it.
        static bool overlay_takes(const MSG *msg);

        using PeekMessageA_fn = BOOL(WINAPI *)(LPMSG, HWND, UINT, UINT, UINT);
        using PeekMessageW_fn = BOOL(WINAPI *)(LPMSG, HWND, UINT, UINT, UINT);
        using GetMessageA_fn = BOOL(WINAPI *)(LPMSG, HWND, UINT, UINT);
        using GetMessageW_fn = BOOL(WINAPI *)(LPMSG, HWND, UINT, UINT);
        using DispatchMessageA_fn = LRESULT(WINAPI *)(const MSG *);
        using DispatchMessageW_fn = LRESULT(WINAPI *)(const MSG *);

        PeekMessageA_fn m_orig_peek_message_a = nullptr;
        PeekMessageW_fn m_orig_peek_message_w = nullptr;
        GetMessageA_fn m_orig_get_message_a = nullptr;
        GetMessageW_fn m_orig_get_message_w = nullptr;
        DispatchMessageA_fn m_orig_dispatch_message_a = nullptr;
        DispatchMessageW_fn m_orig_dispatch_message_w = nullptr;

        // --- Polled keyboard state: three ways to ask the same question ---

        static SHORT WINAPI Hooked_GetAsyncKeyState(int vKey);
        static SHORT WINAPI Hooked_GetKeyState(int vKey);
        static BOOL WINAPI Hooked_GetKeyboardState(PBYTE lpKeyState);

        using GetAsyncKeyState_fn = SHORT(WINAPI *)(int);
        using GetKeyState_fn = SHORT(WINAPI *)(int);
        using GetKeyboardState_fn = BOOL(WINAPI *)(PBYTE);

        GetAsyncKeyState_fn m_orig_get_async_key_state = nullptr;
        GetKeyState_fn m_orig_get_key_state = nullptr;
        GetKeyboardState_fn m_orig_get_keyboard_state = nullptr;

        // --- The cursor: games in mouselook re-centre and confine it every frame ---

        static BOOL WINAPI Hooked_SetCursorPos(int X, int Y);
        static BOOL WINAPI Hooked_ClipCursor(const RECT *lpRect);

        using SetCursorPos_fn = BOOL(WINAPI *)(int, int);
        using ClipCursor_fn = BOOL(WINAPI *)(const RECT *);

        SetCursorPos_fn m_orig_set_cursor_pos = nullptr;
        ClipCursor_fn m_orig_clip_cursor = nullptr;

        // init() and shutdown() can be called from either the loader thread or the panel.
        std::mutex m_mutex;
        bool m_initialized = false;
    };
}
