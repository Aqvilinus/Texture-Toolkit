#pragma once

#include <d3d11.h>
#include <atomic>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mutex>
#include <unordered_map>

namespace TextureToolkit
{
    class D3D11Hook
    {
    public:

        static D3D11Hook &get();

        bool init();
        void shutdown();

        void hook_context(ID3D11DeviceContext *context);
        void hook_device(ID3D11Device *device);

        // Creates a throwaway swapchain to hook the shared IDXGISwapChain::Present vtable slot,
        // so the game's Present is intercepted regardless of how/where it creates its swapchain
        // (flip-model, a factory we never saw created, or a proxy dxgi.dll). Runs once, off the
        // loader lock. See the .cpp for why this is more reliable than hooking factory creation.

        ID3D11Device *get_device() const { return m_device; }

        // The DXGI layer asks these two: whether the game is a D3D11 title at all (its bootstrap
        // fallback must not create a device for a D3D9 game), and whether the overlay is up.
        bool device_hooked() const { return m_device_hooked.load(std::memory_order_acquire); }
        bool overlay_ready() const { return m_imgui_initialized; }

        // Called once per presented frame by the DXGI layer: that is when the back buffer exists
        // and the overlay can be drawn into it.
        void render_imgui(IDXGISwapChain *swapchain);
        HRESULT create_device_and_swapchain_untouched(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                                      const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
                                                      const DXGI_SWAP_CHAIN_DESC *swap_desc, IDXGISwapChain **swapchain,
                                                      ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context) const
        {
            return (m_orig_create_device_and_swapchain != nullptr)
                       ? m_orig_create_device_and_swapchain(adapter, type, software, flags, levels, level_count, sdk,
                                                            swap_desc, swapchain, device, level, context)
                       : E_FAIL;
        }
        ID3D11DeviceContext *get_context() const { return m_context; }

        // Re-entrancy guard for injection
        static thread_local bool s_inside_injection;

    private:
        D3D11Hook() = default;
        ~D3D11Hook();

        typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(
            IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
            const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

        typedef HRESULT(WINAPI *D3D11CreateDevice_t)(
            IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
            ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);


        typedef HRESULT(STDMETHODCALLTYPE *Map_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
        typedef void(STDMETHODCALLTYPE *Unmap_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT);

        static HRESULT WINAPI Hooked_D3D11CreateDeviceAndSwapChain(
            IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
            const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
            const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
            ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);

        static HRESULT WINAPI Hooked_D3D11CreateDevice(
            IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
            const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
            ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);


        static HRESULT STDMETHODCALLTYPE Hooked_Map(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource);
        static void STDMETHODCALLTYPE Hooked_Unmap(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource);

        void init_imgui(IDXGISwapChain *swapchain);

        std::mutex m_mutex;
        bool m_initialized = false;
        bool m_imgui_initialized = false;

        IDXGISwapChain *m_swapchain = nullptr;
        ID3D11Device *m_device = nullptr;
        ID3D11DeviceContext *m_context = nullptr;
        HWND m_hwnd = nullptr;

        D3D11CreateDeviceAndSwapChain_t m_orig_create_device_and_swapchain = nullptr;
        D3D11CreateDevice_t m_orig_create_device = nullptr;

        typedef HRESULT(STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
        CreateTexture2D_t m_orig_create_texture2d = nullptr;
        bool m_context_hooked = false;
        std::atomic<bool> m_device_hooked{false};


        typedef void(STDMETHODCALLTYPE *PSSetShaderResources_t)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
        PSSetShaderResources_t m_orig_ps_set_shader_resources = nullptr;


        typedef HRESULT(STDMETHODCALLTYPE *CreateShaderResourceView_t)(ID3D11Device *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC *, ID3D11ShaderResourceView **);
        CreateShaderResourceView_t m_orig_create_srv = nullptr;

        Map_t m_orig_map = nullptr;
        Unmap_t m_orig_unmap = nullptr;

        static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateShaderResourceView(ID3D11Device *device, ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView);
        static void STDMETHODCALLTYPE Hooked_PSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews);
    };
}
