#pragma once

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>

namespace TextureToolkit
{
    // What the game is rendering with right now. The hooks fill it as they catch each object; the
    // texture code and the overlay read it, so neither has to know which hook owns what -- and the
    // D3D11 half no longer has to be asked for a D3D9 device or a swapchain that is not its own.
    //
    // No reference is taken on anything here, and the atomics below publish the pointer, not the
    // object's lifetime. What keeps them valid is where they are read: every use happens inside a
    // hook the game itself is calling -- CreateTexture2D, Present, the panel drawn from Present --
    // so the game cannot have destroyed the object and still be inside that call. Nothing on the
    // dump worker thread touches a device; it only writes files. Take a ComPtr here if that ever
    // stops being true.
    class RenderBackend
    {
    public:
        static RenderBackend &get();

        void set_d3d11(ID3D11Device *device, ID3D11DeviceContext *context);
        void set_swapchain(IDXGISwapChain *swapchain);
        void set_d3d9(IDirect3DDevice9 *device);
        void set_window(HWND hwnd) { m_hwnd.store(hwnd, std::memory_order_release); }

        ID3D11Device *d3d11_device() const { return m_d3d11_device.load(std::memory_order_acquire); }
        ID3D11DeviceContext *d3d11_context() const { return m_d3d11_context.load(std::memory_order_acquire); }
        IDirect3DDevice9 *d3d9_device() const { return m_d3d9_device.load(std::memory_order_acquire); }
        IDXGISwapChain *swapchain() const { return m_swapchain.load(std::memory_order_acquire); }
        HWND window() const { return m_hwnd.load(std::memory_order_acquire); }

        bool is_d3d11() const { return d3d11_device() != nullptr; }
        bool is_d3d9() const { return d3d9_device() != nullptr; }

    private:
        // Written by whichever hook catches each object first, read from the render thread and the
        // panel. Atomics rather than a lock: these are published once and read constantly.
        std::atomic<ID3D11Device *> m_d3d11_device{nullptr};
        std::atomic<ID3D11DeviceContext *> m_d3d11_context{nullptr};
        std::atomic<IDirect3DDevice9 *> m_d3d9_device{nullptr};
        std::atomic<IDXGISwapChain *> m_swapchain{nullptr};
        std::atomic<HWND> m_hwnd{nullptr};
    };
}
