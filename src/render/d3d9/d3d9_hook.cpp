#include "render/d3d9/d3d9_hook.h"
#include "render/render_backend.h"
#include "core/hook_manager.h"
#include "core/iat_hook.h"
#include "render/d3d9/d3d9_texture_manager.h"
#include "ui/overlay.h"
#include "core/config.h"
#include "input/input_hook.h"
#include "core/logger.h"
#include "core/scoped_flag.h"
#include <imgui.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{
    static WNDPROC g_orig_wndproc = nullptr;

    static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (TextureToolkitUI::is_visible())
        {
            {
                InputHook::PassthroughScope pass;
                ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            }

            if (msg == WM_INPUT)
                return 0;

            if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))
            {
                return 0;
            }
        }

        return CallWindowProc(g_orig_wndproc, hWnd, msg, wParam, lParam);
    }

    struct LockedTextureData
    {
        IDirect3DTexture9 *texture = nullptr;
        UINT width = 0;
        UINT height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        D3DLOCKED_RECT rect = {};
    };

    static thread_local std::unordered_map<IDirect3DTexture9 *, LockedTextureData> s_locked_textures;
    thread_local bool D3D9Hook::s_inside_injection = false;

    D3D9Hook &D3D9Hook::get()
    {
        static D3D9Hook instance;
        return instance;
    }

    D3D9Hook::~D3D9Hook()
    {
        shutdown();
    }

    bool D3D9Hook::init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;

        HMODULE d3d9_module = GetModuleHandleA("d3d9.dll");
        if (d3d9_module == nullptr)
        {
            d3d9_module = LoadLibraryA("d3d9.dll");
        }

        if (d3d9_module != nullptr)
        {
            void *pDirect3DCreate9 = reinterpret_cast<void *>(GetProcAddress(d3d9_module, "Direct3DCreate9"));
            if (pDirect3DCreate9 != nullptr)
            {
                HookManager::get().create_hook(pDirect3DCreate9, &Hooked_Direct3DCreate9, reinterpret_cast<void **>(&m_orig_direct3d_create9));
                IATHook::hook_all_modules("d3d9.dll", "Direct3DCreate9", &Hooked_Direct3DCreate9, reinterpret_cast<void **>(&m_orig_direct3d_create9));
                Logger::get().info("[D3D9Hook] Direct3DCreate9 API & IAT hooks installed successfully.");
            }

            void *pDirect3DCreate9Ex = reinterpret_cast<void *>(GetProcAddress(d3d9_module, "Direct3DCreate9Ex"));
            if (pDirect3DCreate9Ex != nullptr)
            {
                HookManager::get().create_hook(pDirect3DCreate9Ex, &Hooked_Direct3DCreate9Ex, reinterpret_cast<void **>(&m_orig_direct3d_create9_ex));
                IATHook::hook_all_modules("d3d9.dll", "Direct3DCreate9Ex", &Hooked_Direct3DCreate9Ex, reinterpret_cast<void **>(&m_orig_direct3d_create9_ex));
                Logger::get().info("[D3D9Hook] Direct3DCreate9Ex API & IAT hooks installed successfully.");
            }
        }

        m_initialized = true;
        return true;
    }

    void D3D9Hook::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized)
            return;

        if (m_imgui_initialized)
        {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            m_imgui_initialized = false;
        }

        if (m_hwnd && g_orig_wndproc)
        {
            SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
            g_orig_wndproc = nullptr;
        }

        m_initialized = false;
    }

    // --- Entry points ---

    void D3D9Hook::hook_interface(IDirect3D9 *d3d9)
    {
        if (d3d9 == nullptr || m_orig_create_device != nullptr)
            return;

        void **d3d9_vtable = *reinterpret_cast<void ***>(d3d9);
        void *create_device_addr = d3d9_vtable[16]; // IDirect3D9::CreateDevice is index 16

        HookManager::get().create_hook(create_device_addr, &Hooked_CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
        Logger::get().info("[D3D9Hook] Intercepted IDirect3D9::CreateDevice (VTable index 16).");
    }

    void D3D9Hook::hook_device(IDirect3DDevice9 *device)
    {
        if (device == nullptr || m_device != nullptr)
            return;

        m_device = device;
        RenderBackend::get().set_d3d9(device);
        void **vtable = *reinterpret_cast<void ***>(device);

        void *present_addr = vtable[17];
        void *reset_addr = vtable[16];
        void *create_tex_addr = vtable[23];
        void *update_tex_addr = vtable[31]; // IDirect3DDevice9::UpdateTexture
        void *set_tex_addr = vtable[65];

        HookManager::get().create_hook(present_addr, &Hooked_Present, reinterpret_cast<void **>(&m_orig_present));
        HookManager::get().create_hook(reset_addr, &Hooked_Reset, reinterpret_cast<void **>(&m_orig_reset));
        HookManager::get().create_hook(create_tex_addr, &Hooked_CreateTexture, reinterpret_cast<void **>(&m_orig_create_texture));
        HookManager::get().create_hook(update_tex_addr, &Hooked_UpdateTexture, reinterpret_cast<void **>(&m_orig_update_texture));
        HookManager::get().create_hook(set_tex_addr, &Hooked_SetTexture, reinterpret_cast<void **>(&m_orig_set_texture));

        // A game that loads textures through D3DX has the library in the process by now.
        hook_d3dx();

        // Present never fires for a D3D9Ex device; it presents through this separate slot.
        IDirect3DDevice9Ex *device_ex = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&device_ex))) && device_ex != nullptr)
        {
            void **ex_vtable = *reinterpret_cast<void ***>(device_ex);
            void *present_ex_addr = ex_vtable[121]; // IDirect3DDevice9Ex::PresentEx
            HookManager::get().create_hook(present_ex_addr, &Hooked_PresentEx, reinterpret_cast<void **>(&m_orig_present_ex));
            Logger::get().info("[D3D9Hook] Device is D3D9Ex; hooked PresentEx (VTable index 121).");
            device_ex->Release();
        }

        // Nor does either of the above when a game presents through the swap chain instead.
        IDirect3DSwapChain9 *swapchain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapchain)) && swapchain != nullptr)
        {
            void **sc_vtable = *reinterpret_cast<void ***>(swapchain);
            void *sc_present_addr = sc_vtable[3]; // IDirect3DSwapChain9::Present
            HookManager::get().create_hook(sc_present_addr, &Hooked_SwapChainPresent, reinterpret_cast<void **>(&m_orig_swapchain_present));
            Logger::get().info("[D3D9Hook] Hooked IDirect3DSwapChain9::Present (VTable index 3).");
            swapchain->Release();
        }

        Logger::get().info("[D3D9Hook] REAL GAME DEVICE INTERCEPTED! VTable hooks active on game IDirect3DDevice9.");
    }

    IDirect3D9 *WINAPI D3D9Hook::Hooked_Direct3DCreate9(UINT SDKVersion)
    {
        IDirect3D9 *d3d9 = nullptr;
        if (get().m_orig_direct3d_create9 != nullptr)
        {
            d3d9 = get().m_orig_direct3d_create9(SDKVersion);
        }

        if (d3d9 != nullptr)
        {
            get().hook_interface(d3d9);
        }
        return d3d9;
    }

    HRESULT WINAPI D3D9Hook::Hooked_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D)
    {
        HRESULT hr = E_FAIL;
        if (get().m_orig_direct3d_create9_ex != nullptr)
        {
            hr = get().m_orig_direct3d_create9_ex(SDKVersion, ppD3D);
        }

        if (SUCCEEDED(hr) && ppD3D != nullptr && *ppD3D != nullptr)
        {
            get().hook_interface(*ppD3D);
        }
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_CreateDevice(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnDeviceInterface)
    {
        Logger::get().info("[D3D9Hook] IDirect3D9::CreateDevice was called by the game!");
        
        HRESULT hr = get().m_orig_create_device(d3d9, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnDeviceInterface);

        if (SUCCEEDED(hr) && ppReturnDeviceInterface != nullptr && *ppReturnDeviceInterface != nullptr)
        {
            Logger::get().info("[D3D9Hook] CreateDevice succeeded. Hooking returned device...");
            get().hook_device(*ppReturnDeviceInterface);
        }
        else
        {
            Logger::get().error("[D3D9Hook] CreateDevice failed with HRESULT: " + std::to_string(hr));
        }

        return hr;
    }

    // --- Presentation ---

    thread_local bool D3D9Hook::s_presenting = false;

    void D3D9Hook::present_frame(IDirect3DDevice9 *device, PresentSource source)
    {
        if (device == nullptr)
            return;

        m_device = device;
        m_frames.fetch_add(1, std::memory_order_relaxed);

        static bool s_logged[3] = {};
        const size_t index = static_cast<size_t>(source);
        if (!s_logged[index])
        {
            s_logged[index] = true;
            static const char *kNames[] = { "Device::Present", "DeviceEx::PresentEx", "SwapChain::Present" };
            Logger::get().info(std::string("[D3D9Hook] First frame via ") + kNames[index] + "; overlay path active.");
        }

        render_imgui(device);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_Present(IDirect3DDevice9 *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
    {
        if (s_presenting)
            return get().m_orig_present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

        ScopedFlag presenting(s_presenting);
        get().present_frame(device, PresentSource::Device);
        return get().m_orig_present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_PresentEx(IDirect3DDevice9Ex *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
    {
        if (s_presenting)
            return get().m_orig_present_ex(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);

        ScopedFlag presenting(s_presenting);
        get().present_frame(device, PresentSource::DeviceEx);
        return get().m_orig_present_ex(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SwapChainPresent(IDirect3DSwapChain9 *swapchain, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
    {
        if (s_presenting)
            return get().m_orig_swapchain_present(swapchain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);

        ScopedFlag presenting(s_presenting);
        get().present_frame(get().m_device, PresentSource::SwapChain);
        return get().m_orig_swapchain_present(swapchain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_Reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters)
    {
        if (get().m_imgui_initialized)
        {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        HRESULT hr = get().m_orig_reset(device, pPresentationParameters);

        if (SUCCEEDED(hr) && get().m_imgui_initialized)
        {
            ImGui_ImplDX9_CreateDeviceObjects();
        }

        return hr;
    }

    // --- Textures ---

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_CreateTexture(IDirect3DDevice9 *device, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
    {
        if (s_inside_injection)
            return get().m_orig_create_texture(device, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);

        Logger::get().debug("[D3D9Hook] Hooked_CreateTexture called: Width=" + std::to_string(Width) + ", Height=" + std::to_string(Height) + ", Format=" + std::to_string(static_cast<uint32_t>(Format)));
        HRESULT hr = get().m_orig_create_texture(device, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);

        if (SUCCEEDED(hr) && ppTexture != nullptr && *ppTexture != nullptr)
        {
            IDirect3DTexture9 *tex = *ppTexture;
            void **vtable = *reinterpret_cast<void ***>(tex);

            void *lock_addr = vtable[19];
            void *unlock_addr = vtable[20];

            if (get().m_orig_lock_rect == nullptr)
            {
                HookManager::get().create_hook(lock_addr, &Hooked_LockRect, reinterpret_cast<void **>(&get().m_orig_lock_rect));
                HookManager::get().create_hook(unlock_addr, &Hooked_UnlockRect, reinterpret_cast<void **>(&get().m_orig_unlock_rect));
                Logger::get().info("[D3D9Hook] Hooked LockRect and UnlockRect on the first created texture.");
            }

            IDirect3DSurface9 *pSurface = nullptr;
            if (SUCCEEDED(tex->GetSurfaceLevel(0, &pSurface)) && pSurface != nullptr)
            {
                void **surface_vtable = *reinterpret_cast<void ***>(pSurface);
                void *surf_lock_addr = surface_vtable[13];
                void *surf_unlock_addr = surface_vtable[14];

                if (get().m_orig_surface_lock_rect == nullptr)
                {
                    HookManager::get().create_hook(surf_lock_addr, &Hooked_SurfaceLockRect, reinterpret_cast<void **>(&get().m_orig_surface_lock_rect));
                    HookManager::get().create_hook(surf_unlock_addr, &Hooked_SurfaceUnlockRect, reinterpret_cast<void **>(&get().m_orig_surface_unlock_rect));
                    Logger::get().info("[D3D9Hook] Hooked Surface LockRect and UnlockRect on the first surface.");
                }

                pSurface->Release();
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_LockRect(IDirect3DTexture9 *texture, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
    {
        HRESULT hr = get().m_orig_lock_rect(texture, Level, pLockedRect, pRect, Flags);

        if (s_inside_injection)
            return hr;

        if (SUCCEEDED(hr) && Level == 0 && pLockedRect != nullptr && pLockedRect->pBits != nullptr)
        {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(texture->GetLevelDesc(0, &desc)))
            {
                static int s_logged_locks = 0;
                if (s_logged_locks < 20)
                {
                    s_logged_locks++;
                    std::string has_rect = (pRect != nullptr) ? "Yes" : "No";
                    Logger::get().debug("[D3D9Hook] Hooked_LockRect: Texture=0x" + Logger::fmt("%p", (texture)) + " Width=" + std::to_string(desc.Width) + " Height=" + std::to_string(desc.Height) + " pRect=" + has_rect);
                }

                // A partial lock shows only some of the pixels, which would hash as a different texture.
                if (pRect != nullptr)
                {
                    UINT rect_w = pRect->right - pRect->left;
                    UINT rect_h = pRect->bottom - pRect->top;
                    if (rect_w != desc.Width || rect_h != desc.Height)
                    {
                        return hr;
                    }
                }

                // Rewritten constantly (UI, fonts), so a content hash identifies nothing.
                if ((desc.Usage & D3DUSAGE_DYNAMIC) != 0)
                {
                    return hr;
                }

                LockedTextureData data;
                data.texture = texture;
                data.width = desc.Width;
                data.height = desc.Height;
                data.format = desc.Format;
                data.rect = *pLockedRect;

                s_locked_textures[texture] = data;
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_UnlockRect(IDirect3DTexture9 *texture, UINT Level)
    {
        if (s_inside_injection)
            return get().m_orig_unlock_rect(texture, Level);

        if (Level == 0)
        {
            auto it = s_locked_textures.find(texture);
            if (it != s_locked_textures.end())
            {
                LockedTextureData &data = it->second;
                static int s_logged_unlocks = 0;
                if (s_logged_unlocks < 20)
                {
                    s_logged_unlocks++;
                    Logger::get().debug("[D3D9Hook] Hooked_UnlockRect: Registering texture=0x" + Logger::fmt("%p", (texture)));
                }

                // Pitch is signed here and unsigned from here on, so a negative one would
                // become an enormous copy length rather than a rejected lock.
                if (data.rect.Pitch > 0)
                {
                    D3D9TextureManager::get().register_texture9(
                        get().m_device,
                        texture,
                        data.rect.pBits,
                        data.width,
                        data.height,
                        data.format,
                        static_cast<UINT>(data.rect.Pitch)
                    );
                }

                s_locked_textures.erase(it);
            }
        }

        return get().m_orig_unlock_rect(texture, Level);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage, IDirect3DBaseTexture9 *pTexture)
    {
        static int s_logged_set_texture_calls = 0;
        if (s_logged_set_texture_calls < 20 && pTexture != nullptr)
        {
            s_logged_set_texture_calls++;
            Logger::get().debug("[D3D9Hook] Hooked_SetTexture: Stage=" + std::to_string(Stage) + " pTexture=0x" + Logger::fmt("%p", (pTexture)));
        }

        IDirect3DBaseTexture9 *pReplacement = D3D9TextureManager::get().get_replacement_texture9(pTexture);
        return get().m_orig_set_texture(device, Stage, pReplacement);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_UpdateTexture(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
    {
        HRESULT hr = get().m_orig_update_texture(device, pSourceTexture, pDestinationTexture);

        // Art is loaded into a SYSTEMMEM texture, hashed on unlock, then copied into the DEFAULT
        // texture that actually gets bound -- so the tag has to travel with it.
        if (SUCCEEDED(hr) && !s_inside_injection)
            D3D9TextureManager::get().copy_tag9(pSourceTexture, pDestinationTexture);

        return hr;
    }

    // --- D3DX ---

    void D3D9Hook::hook_d3dx()
    {
        if (m_orig_d3dx_from_memory != nullptr)
            return;

        HMODULE module = GetModuleHandleW(L"d3dx9_43.dll");
        if (module == nullptr)
            return;

        struct Entry
        {
            const char *name;
            void *detour;
            void **original;
        };

        const Entry entries[] = {
            { "D3DXCreateTextureFromFileInMemoryEx", &Hooked_D3DXCreateTextureFromFileInMemoryEx, reinterpret_cast<void **>(&m_orig_d3dx_from_memory) },
            { "D3DXCreateTextureFromFileExW", &Hooked_D3DXCreateTextureFromFileExW, reinterpret_cast<void **>(&m_orig_d3dx_from_file_w) },
            { "D3DXCreateTextureFromFileExA", &Hooked_D3DXCreateTextureFromFileExA, reinterpret_cast<void **>(&m_orig_d3dx_from_file_a) },
        };

        size_t installed = 0;
        for (const Entry &entry : entries)
        {
            if (void *address = reinterpret_cast<void *>(GetProcAddress(module, entry.name)))
            {
                HookManager::get().create_hook(address, entry.detour, entry.original);
                ++installed;
            }
        }

        if (installed != 0)
            Logger::get().info("[D3D9Hook] Watching " + std::to_string(installed) + " D3DX texture entry point(s); textures loaded through them arrive with a full mip chain.");
    }

    namespace
    {
        void adopt_d3dx_texture(IDirect3DDevice9 *device, IDirect3DTexture9 **texture)
        {
            if (D3D9Hook::s_inside_injection || texture == nullptr || *texture == nullptr)
                return;

            D3D9TextureManager::get().register_from_d3dx(device, *texture);
        }
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileInMemoryEx(IDirect3DDevice9 *device, LPCVOID pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, void *pSrcInfo, PALETTEENTRY *pPalette, IDirect3DTexture9 **ppTexture)
    {
        const HRESULT hr = get().m_orig_d3dx_from_memory(device, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
        if (SUCCEEDED(hr))
            adopt_d3dx_texture(device, ppTexture);
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileExW(IDirect3DDevice9 *device, LPCWSTR pSrcFile, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, void *pSrcInfo, PALETTEENTRY *pPalette, IDirect3DTexture9 **ppTexture)
    {
        const HRESULT hr = get().m_orig_d3dx_from_file_w(device, pSrcFile, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
        if (SUCCEEDED(hr))
            adopt_d3dx_texture(device, ppTexture);
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileExA(IDirect3DDevice9 *device, LPCSTR pSrcFile, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, void *pSrcInfo, PALETTEENTRY *pPalette, IDirect3DTexture9 **ppTexture)
    {
        const HRESULT hr = get().m_orig_d3dx_from_file_a(device, pSrcFile, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
        if (SUCCEEDED(hr))
            adopt_d3dx_texture(device, ppTexture);
        return hr;
    }

    // --- Surfaces ---

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SurfaceLockRect(IDirect3DSurface9 *surface, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
    {
        HRESULT hr = get().m_orig_surface_lock_rect(surface, pLockedRect, pRect, Flags);

        if (s_inside_injection)
            return hr;

        if (SUCCEEDED(hr) && pLockedRect != nullptr && pLockedRect->pBits != nullptr)
        {
            IDirect3DTexture9 *texture = nullptr;
            if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&texture))) && texture != nullptr)
            {
                D3DSURFACE_DESC desc = {};
                if (SUCCEEDED(surface->GetDesc(&desc)))
                {
                    static int s_logged_surf_locks = 0;
                    if (s_logged_surf_locks < 20)
                    {
                        s_logged_surf_locks++;
                        std::string has_rect = (pRect != nullptr) ? "Yes" : "No";
                        Logger::get().debug("[D3D9Hook] Hooked_SurfaceLockRect: Surface=0x" + Logger::fmt("%p", (surface)) + " ParentTexture=0x" + Logger::fmt("%p", (texture)) + " Width=" + std::to_string(desc.Width) + " Height=" + std::to_string(desc.Height) + " pRect=" + has_rect);
                    }

                    if (pRect != nullptr)
                    {
                        UINT rect_w = pRect->right - pRect->left;
                        UINT rect_h = pRect->bottom - pRect->top;
                        if (rect_w != desc.Width || rect_h != desc.Height)
                        {
                            texture->Release();
                            return hr;
                        }
                    }

                    // Only level 0. A surface reached through GetSurfaceLevel(n) carries mip n,
                    // and registering it would hash those pixels and stamp the result on the parent
                    // texture -- so the tag the bind hook reads would name a mip instead of the
                    // texture, and no replacement would ever be found for it.
                    D3DSURFACE_DESC top = {};
                    if (FAILED(texture->GetLevelDesc(0, &top)) ||
                        top.Width != desc.Width || top.Height != desc.Height)
                    {
                        texture->Release();
                        return hr;
                    }

                    // Rewritten constantly (UI, fonts), so a content hash identifies nothing.
                    if ((desc.Usage & D3DUSAGE_DYNAMIC) == 0)
                    {
                        LockedTextureData data;
                        data.texture = texture;
                        data.width = desc.Width;
                        data.height = desc.Height;
                        data.format = desc.Format;
                        data.rect = *pLockedRect;

                        s_locked_textures[texture] = data;
                    }
                }
                texture->Release();
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SurfaceUnlockRect(IDirect3DSurface9 *surface)
    {
        if (s_inside_injection)
            return get().m_orig_surface_unlock_rect(surface);

        IDirect3DTexture9 *texture = nullptr;
        if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&texture))) && texture != nullptr)
        {
            auto it = s_locked_textures.find(texture);
            if (it != s_locked_textures.end())
            {
                LockedTextureData &data = it->second;
                static int s_logged_surf_unlocks = 0;
                if (s_logged_surf_unlocks < 20)
                {
                    s_logged_surf_unlocks++;
                    Logger::get().debug("[D3D9Hook] Hooked_SurfaceUnlockRect: Registering parent texture=0x" + Logger::fmt("%p", (texture)));
                }

                // Pitch is signed here and unsigned from here on, so a negative one would
                // become an enormous copy length rather than a rejected lock.
                if (data.rect.Pitch > 0)
                {
                    D3D9TextureManager::get().register_texture9(
                        get().m_device,
                        texture,
                        data.rect.pBits,
                        data.width,
                        data.height,
                        data.format,
                        static_cast<UINT>(data.rect.Pitch)
                    );
                }

                s_locked_textures.erase(it);
            }
            texture->Release();
        }

        return get().m_orig_surface_unlock_rect(surface);
    }

    // --- The overlay ---

    void D3D9Hook::init_imgui(IDirect3DDevice9 *device)
    {
        if (m_imgui_initialized || device == nullptr)
            return;

        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow != nullptr)
        {
            m_hwnd = params.hFocusWindow;
        }

        if (m_hwnd == nullptr)
        {
            m_hwnd = GetActiveWindow();
        }

        if (m_hwnd != nullptr)
        {
            g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hooked_WndProc)));
        }

        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        wchar_t exe_path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));
        std::filesystem::path game_dir = std::filesystem::path(exe_path).parent_path();
        std::filesystem::path imgui_ini = game_dir / ConfigManager::get().get_config().resource_root / "imgui.ini";

        static std::string ini_path_str = imgui_ini.string();
        ImGui::GetIO().IniFilename = ini_path_str.c_str();

        ImGui_ImplWin32_Init(m_hwnd);
        ImGui_ImplDX9_Init(device);

        m_imgui_initialized = true;
        Logger::get().info("[D3D9Hook] Dear ImGui initialized natively for real game DirectX 9 device.");
    }

    void D3D9Hook::render_imgui(IDirect3DDevice9 *device)
    {
        // Same gate as the D3D11 side. Without it the panel still opened here -- the hotkey poll
        // falls back to the unhooked GetAsyncKeyState -- while the input detours it needs were
        // never installed, so the game went on receiving every keystroke typed into it.
        if (!ConfigManager::get().get_config().enable_overlay)
        {
            D3D9TextureManager::get().on_frame();
            return;
        }

        if (!m_imgui_initialized)
        {
            init_imgui(device);
        }

        if (!m_imgui_initialized)
            return;

        uint32_t toggle_key = ConfigManager::get().get_config().hotkey;
        static bool s_key_was_down = false;
        bool key_is_down = (InputHook::real_async_key_state(toggle_key) & 0x8000) != 0;
        if (key_is_down && !s_key_was_down)
        {
            TextureToolkitUI::toggle_visibility();
            bool visible = TextureToolkitUI::is_visible();
            Logger::get().info("[UI] Direct hotkey poll triggered UI toggle. Visibility = " + std::to_string(visible));
        }
        s_key_was_down = key_is_down;

        D3D9TextureManager::get().on_frame();

        InputHook::PassthroughScope pass;

        ImGuiIO &io = ImGui::GetIO();
        if (TextureToolkitUI::is_visible())
        {
            TextureToolkitUI::feed_overlay_mouse(m_hwnd);
        }
        else
        {
            // Clearing this is the whole restore: the backend puts the OS cursor back.
            io.MouseDrawCursor = false;
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        TextureToolkitUI::draw_ui();

        ImGui::EndFrame();
        ImGui::Render();

        
        // Present runs outside a scene, so open one -- and close it only if we opened it, since
        // BeginScene fails when the game already has one and EndScene would then close theirs.
        if (SUCCEEDED(device->BeginScene()))
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }
    }
}
