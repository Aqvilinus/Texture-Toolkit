#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <atomic>
#include <mutex>

namespace TextureToolkit
{
    // The D3D11 side: a device, its context, and the textures they create. Replacement happens at
    // creation, the one moment a texture's pixels are known and nothing has drawn with it yet.
    // Presentation is not here -- that is shared with D3D10 and 12 and lives in DXGIHook, which
    // calls render_imgui once a frame.
    class D3D11Hook
    {
    public:
        static D3D11Hook &get();

        bool init();
        void shutdown();

        // The device and its context arrive when the game creates them, so each is hooked then.
        void hook_device(ID3D11Device *device);
        void hook_context(ID3D11DeviceContext *context);

        ID3D11Device *get_device() const { return m_device; }
        ID3D11DeviceContext *get_context() const { return m_context; }

        // Both asked by the DXGI layer: whether this is a D3D11 game at all, since its bootstrap
        // must not create a device for a D3D9 one, and whether the overlay is up.
        bool device_hooked() const { return m_device_hooked.load(std::memory_order_acquire); }
        bool overlay_ready() const { return m_imgui_initialized; }

        // Called once per presented frame, which is when the back buffer exists to draw into.
        void render_imgui(IDXGISwapChain *swapchain);

        // Lets the DXGI bootstrap build its throwaway device without re-entering our own detour.
        HRESULT original_create_device_and_swapchain(
            IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
            const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
            const DXGI_SWAP_CHAIN_DESC *swap_desc, IDXGISwapChain **swapchain,
            ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context) const;

        // Set while we create or read back our own textures, so the hooks below hand those
        // straight through. Thread local: a game creating textures on another thread stays tracked.
        static thread_local bool s_inside_injection;

    private:
        D3D11Hook() = default;
        ~D3D11Hook();

        // Signatures must match the D3D11 headers exactly: create_hook passes each trampoline
        // through void **, so a mismatch compiles and only shows up as a corrupted stack.

        // --- Entry points: the exported functions that hand out a device ---

        using D3D11CreateDevice_fn = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
        using D3D11CreateDeviceAndSwapChain_fn = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

        static HRESULT WINAPI Hooked_D3D11CreateDevice(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);
        static HRESULT WINAPI Hooked_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);

        D3D11CreateDevice_fn m_orig_create_device = nullptr;
        D3D11CreateDeviceAndSwapChain_fn m_orig_create_device_and_swapchain = nullptr;

        // --- Textures: the pixels arrive with the creation call, so replacement happens there ---

        using CreateTexture2D_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
        using CreateShaderResourceView_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11Device *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC *, ID3D11ShaderResourceView **);
        using PSSetShaderResources_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);

        static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateShaderResourceView(ID3D11Device *device, ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView);
        static void STDMETHODCALLTYPE Hooked_PSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews);

        CreateTexture2D_fn m_orig_create_texture2d = nullptr;
        CreateShaderResourceView_fn m_orig_create_shader_resource_view = nullptr;
        PSSetShaderResources_fn m_orig_ps_set_shader_resources = nullptr;

        // --- Uploads: a texture created empty and filled afterwards, when TrackMapUnmap is on ---

        using Map_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
        using Unmap_fn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);

        static HRESULT STDMETHODCALLTYPE Hooked_Map(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource);
        static void STDMETHODCALLTYPE Hooked_Unmap(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource);

        Map_fn m_orig_map = nullptr;
        Unmap_fn m_orig_unmap = nullptr;

        // --- The overlay, drawn from render_imgui ---

        void init_imgui(IDXGISwapChain *swapchain);

        bool m_imgui_initialized = false;
        ID3D11Device *m_device = nullptr;
        ID3D11DeviceContext *m_context = nullptr;
        HWND m_hwnd = nullptr;

        // init() and shutdown() can be called from either the loader thread or the panel.
        std::mutex m_mutex;
        bool m_initialized = false;

        // Read by the DXGI bootstrap thread while this one is still installing hooks.
        std::atomic<bool> m_device_hooked{false};
        bool m_context_hooked = false;
    };
}
