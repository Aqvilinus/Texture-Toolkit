#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>

namespace TextureToolkit
{
    // Presentation: factories, swapchains and Present. This layer is shared by D3D10, 11 and 12, so
    // it knows nothing about textures -- it only reports the live swapchain to the render backend
    // and asks the D3D11 side to draw the overlay once per frame.
    class DXGIHook
    {
    public:
        static DXGIHook &get();

        bool init();
        void hook_swapchain(IDXGISwapChain *swapchain);
        void hook_dxgi_factory(IDXGIFactory *factory);


        // Published before any teardown begins. The bootstrap thread and hook_swapchain both read
        // it, so nothing installs a detour once shutdown has started.
        void begin_shutdown() { m_running.store(false, std::memory_order_release); }
        bool running() const { return m_running.load(std::memory_order_acquire); }

        // Set the moment a Present detour is in place, so the bootstrap thread can test it without
        // racing the function pointer it is written next to.
        bool present_installed() const { return m_present_installed.load(std::memory_order_acquire); }

    private:
        DXGIHook() = default;

        // Runs on its own thread: nothing happens unless a D3D11 game turns up whose swapchain was
        // never seen the normal way, in which case a throwaway swapchain gives us the shared
        // Present slot. Deferred off DllMain because creating a device under the loader lock can
        // deadlock.
        void bootstrap_present();

        static HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory);
        static HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory);
        static HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory *factory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain);
        static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain *swapchain, UINT SyncInterval, UINT Flags);

        typedef HRESULT(WINAPI *CreateDXGIFactory_t)(REFIID, void **);
        typedef HRESULT(WINAPI *CreateDXGIFactory2_t)(UINT, REFIID, void **);
        typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChain_t)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
        typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainForHwnd_t)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
        typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);

        CreateDXGIFactory_t m_orig_create_dxgi_factory = nullptr;
        CreateDXGIFactory_t m_orig_create_dxgi_factory1 = nullptr;
        CreateDXGIFactory2_t m_orig_create_dxgi_factory2 = nullptr;
        CreateSwapChain_t m_orig_create_swapchain = nullptr;
        CreateSwapChainForHwnd_t m_orig_create_swapchain_for_hwnd = nullptr;
        Present_t m_orig_present = nullptr;

        IDXGISwapChain *m_swapchain = nullptr;
        std::atomic<bool> m_running{true};
        std::atomic<bool> m_present_installed{false};
    };
}
