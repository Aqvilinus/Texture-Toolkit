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

        // Factories and swapchains appear one at a time, so each is hooked as the game creates it.
        void hook_factory(IDXGIFactory *factory);
        void hook_swapchain(IDXGISwapChain *swapchain);

        // Published before any teardown begins. The bootstrap thread and hook_swapchain both read
        // it, so nothing installs a detour once shutdown has started.
        void begin_shutdown() { m_running.store(false, std::memory_order_release); }
        bool running() const { return m_running.load(std::memory_order_acquire); }

        // Set the moment a Present detour is in place, so the bootstrap thread can test it without
        // racing the function pointer it is written next to.
        bool present_installed() const { return m_present_installed.load(std::memory_order_acquire); }

    private:
        DXGIHook() = default;

        // Signatures must match the DXGI headers exactly: create_hook passes each trampoline
        // through void **, so a mismatch compiles and only shows up as a corrupted stack.

        // --- Factories: three exported entry points, any of which a game may call ---

        using CreateDXGIFactory_fn = HRESULT(WINAPI *)(REFIID, void **);
        using CreateDXGIFactory2_fn = HRESULT(WINAPI *)(UINT, REFIID, void **);

        static HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory);
        static HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory);
        static HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);

        CreateDXGIFactory_fn m_orig_create_dxgi_factory = nullptr;
        CreateDXGIFactory_fn m_orig_create_dxgi_factory1 = nullptr;
        CreateDXGIFactory2_fn m_orig_create_dxgi_factory2 = nullptr;

        // --- Swapchains: however the game asks a factory for one ---

        using CreateSwapChain_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
        using CreateSwapChainForHwnd_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

        static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory *factory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain);

        CreateSwapChain_fn m_orig_create_swapchain = nullptr;
        CreateSwapChainForHwnd_fn m_orig_create_swapchain_for_hwnd = nullptr;

        // Runs on its own thread: nothing happens unless a D3D11 game turns up whose swapchain was
        // never seen the normal way, in which case a throwaway swapchain gives us the shared
        // Present slot. Deferred off DllMain because creating a device under the loader lock can
        // deadlock.
        void bootstrap_present();

        // --- Present: one vtable slot, shared by every swapchain in the process ---

        using Present_fn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);

        static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain *swapchain, UINT SyncInterval, UINT Flags);

        Present_fn m_orig_present = nullptr;

        // The swapchain the overlay bound to; a second one presenting is worth a warning.
        IDXGISwapChain *m_swapchain = nullptr;

        std::atomic<bool> m_running{true};
        std::atomic<bool> m_present_installed{false};
    };
}
