#pragma once

#include <d3d9.h>
#include <mutex>
#include <unordered_map>

namespace TextureToolkit
{
    // D3D9 has three ways to present and a game may use any of them. The device call reaches the
    // swapchain internally, so one frame can arrive here twice.
    enum class PresentSource
    {
        Device,
        DeviceEx,
        SwapChain,
    };

    // Textures are recognised by the pixels the game uploads, not by the file they came from, so
    // an engine with its own archive format is covered where a D3DX hook sees nothing. The cost is
    // the other direction: whatever the CPU never locks -- UpdateSurface, StretchRect, render
    // targets, anything produced on the GPU -- does not exist as far as this branch is concerned.
    class D3D9Hook
    {
    public:
        static D3D9Hook &get();

        bool init();
        void shutdown();

        // D3D9 objects appear one at a time, so each is hooked as the game creates it.
        void hook_interface(IDirect3D9 *d3d9);
        void hook_device(IDirect3DDevice9 *device);

        IDirect3DDevice9 *get_device() const { return m_device; }

        // Set while we create or lock our own replacement textures, so the hooks below hand those
        // straight through. Thread local: a game uploading on another thread must stay tracked.
        static thread_local bool s_inside_injection;

    private:
        D3D9Hook() = default;
        ~D3D9Hook();

        // Signatures must match the D3D9 headers exactly: create_hook passes each trampoline
        // through void **, so a mismatch compiles and only shows up as a corrupted stack.

        // --- Entry points ---

        using Direct3DCreate9_fn = IDirect3D9 *(WINAPI *)(UINT);
        using Direct3DCreate9Ex_fn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
        using CreateDevice_fn = HRESULT(WINAPI *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);

        static IDirect3D9 *WINAPI Hooked_Direct3DCreate9(UINT SDKVersion);
        static HRESULT WINAPI Hooked_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D);
        static HRESULT WINAPI Hooked_CreateDevice(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface);

        Direct3DCreate9_fn m_orig_direct3d_create9 = nullptr;
        Direct3DCreate9Ex_fn m_orig_direct3d_create9_ex = nullptr;
        CreateDevice_fn m_orig_create_device = nullptr;

        // --- Presentation and device loss ---

        using Present_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
        using PresentEx_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
        using SwapChainPresent_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
        using Reset_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

        static HRESULT STDMETHODCALLTYPE Hooked_Present(IDirect3DDevice9 *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion);
        static HRESULT STDMETHODCALLTYPE Hooked_PresentEx(IDirect3DDevice9Ex *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags);
        static HRESULT STDMETHODCALLTYPE Hooked_SwapChainPresent(IDirect3DSwapChain9 *swapchain, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags);
        static HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters);

        // All three entry points funnel here, so the frame is drawn once.
        void present_frame(IDirect3DDevice9 *device, PresentSource source);

        // Held across the original Present, not just around our drawing: the device call reaches
        // the swapchain internally, and without that span the inner call counts as a second frame.
        // Thread local, or a game presenting from two threads suppresses its own overlay.
        static thread_local bool s_presenting;

        Present_fn m_orig_present = nullptr;
        PresentEx_fn m_orig_present_ex = nullptr;
        SwapChainPresent_fn m_orig_swapchain_present = nullptr;
        Reset_fn m_orig_reset = nullptr;

        // --- Textures ---

        using CreateTexture_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
        using LockRect_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
        using UnlockRect_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT);
        using SetTexture_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
        using UpdateTexture_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);

        static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture(IDirect3DDevice9 *device, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle);
        static HRESULT STDMETHODCALLTYPE Hooked_LockRect(IDirect3DTexture9 *texture, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
        static HRESULT STDMETHODCALLTYPE Hooked_UnlockRect(IDirect3DTexture9 *texture, UINT Level);
        static HRESULT STDMETHODCALLTYPE Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage, IDirect3DBaseTexture9 *pTexture);
        static HRESULT STDMETHODCALLTYPE Hooked_UpdateTexture(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture);

        CreateTexture_fn m_orig_create_texture = nullptr;
        LockRect_fn m_orig_lock_rect = nullptr;
        UnlockRect_fn m_orig_unlock_rect = nullptr;
        SetTexture_fn m_orig_set_texture = nullptr;
        UpdateTexture_fn m_orig_update_texture = nullptr;

        // --- Surfaces: a texture reached through its surface takes this path instead ---

        using SurfaceLockRect_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
        using SurfaceUnlockRect_fn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSurface9 *);

        static HRESULT STDMETHODCALLTYPE Hooked_SurfaceLockRect(IDirect3DSurface9 *surface, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
        static HRESULT STDMETHODCALLTYPE Hooked_SurfaceUnlockRect(IDirect3DSurface9 *surface);

        SurfaceLockRect_fn m_orig_surface_lock_rect = nullptr;
        SurfaceUnlockRect_fn m_orig_surface_unlock_rect = nullptr;

        // --- The overlay ---

        void init_imgui(IDirect3DDevice9 *device);
        void render_imgui(IDirect3DDevice9 *device);

        bool m_imgui_initialized = false;
        IDirect3DDevice9 *m_device = nullptr;
        HWND m_hwnd = nullptr;

        // init() and shutdown() can be called from either the loader thread or the panel.
        std::mutex m_mutex;
        bool m_initialized = false;
    };
}
