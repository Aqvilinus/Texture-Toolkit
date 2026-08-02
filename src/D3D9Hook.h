#pragma once

#include <d3d9.h>
#include <mutex>
#include <unordered_map>

namespace TextureToolkit
{
    class D3D9Hook
    {
    public:
        static D3D9Hook &get();

        bool init();
        void shutdown();

        void hook_d3d9_interface(IDirect3D9 *d3d9);
        void hook_device_interface(IDirect3DDevice9 *device);

        IDirect3DDevice9 *get_device() const { return m_device; }

        // Re-entrancy guard: set true while creating replacement textures
        // to prevent our hooks from re-entering the injection path
        static thread_local bool s_inside_injection;

    private:
        D3D9Hook() = default;
        ~D3D9Hook();

        typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
        typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
        typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
        typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
        typedef HRESULT(STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
        typedef HRESULT(STDMETHODCALLTYPE *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
        typedef HRESULT(STDMETHODCALLTYPE *LockRect_t)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
        typedef HRESULT(STDMETHODCALLTYPE *UnlockRect_t)(IDirect3DTexture9 *, UINT);
        typedef HRESULT(STDMETHODCALLTYPE *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
        typedef HRESULT(STDMETHODCALLTYPE *UpdateTexture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);

        // Surface Hooks
        typedef HRESULT(STDMETHODCALLTYPE *SurfaceLockRect_t)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
        typedef HRESULT(STDMETHODCALLTYPE *SurfaceUnlockRect_t)(IDirect3DSurface9 *);

        static IDirect3D9 *WINAPI Hooked_Direct3DCreate9(UINT SDKVersion);
        static HRESULT WINAPI Hooked_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D);
        static HRESULT WINAPI Hooked_CreateDevice(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnDeviceInterface);
        static HRESULT STDMETHODCALLTYPE Hooked_Present(IDirect3DDevice9 *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion);
        static HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture(IDirect3DDevice9 *device, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle);
        static HRESULT STDMETHODCALLTYPE Hooked_LockRect(IDirect3DTexture9 *texture, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
        static HRESULT STDMETHODCALLTYPE Hooked_UnlockRect(IDirect3DTexture9 *texture, UINT Level);
        static HRESULT STDMETHODCALLTYPE Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage, IDirect3DBaseTexture9 *pTexture);
        static HRESULT STDMETHODCALLTYPE Hooked_UpdateTexture(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture);

        static HRESULT STDMETHODCALLTYPE Hooked_SurfaceLockRect(IDirect3DSurface9 *surface, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
        static HRESULT STDMETHODCALLTYPE Hooked_SurfaceUnlockRect(IDirect3DSurface9 *surface);

        void init_imgui(IDirect3DDevice9 *device);
        void render_imgui(IDirect3DDevice9 *device);

        std::mutex m_mutex;
        bool m_initialized = false;
        bool m_imgui_initialized = false;

        IDirect3DDevice9 *m_device = nullptr;
        HWND m_hwnd = nullptr;

        Direct3DCreate9_t m_orig_direct3d_create9 = nullptr;
        Direct3DCreate9Ex_t m_orig_direct3d_create9_ex = nullptr;
        CreateDevice_t m_orig_create_device = nullptr;
        Present_t m_orig_present = nullptr;
        Reset_t m_orig_reset = nullptr;
        CreateTexture_t m_orig_create_texture = nullptr;
        LockRect_t m_orig_lock_rect = nullptr;
        UnlockRect_t m_orig_unlock_rect = nullptr;
        SetTexture_t m_orig_set_texture = nullptr;
        UpdateTexture_t m_orig_update_texture = nullptr;

        SurfaceLockRect_t m_orig_surface_lock_rect = nullptr;
        SurfaceUnlockRect_t m_orig_surface_unlock_rect = nullptr;
    };
}
