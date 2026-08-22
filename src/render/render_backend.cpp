#include "render/render_backend.h"

#include "core/logger.h"

namespace TextureToolkit
{
    RenderBackend &RenderBackend::get()
    {
        static RenderBackend instance;
        return instance;
    }

    void RenderBackend::set_d3d11(ID3D11Device *device, ID3D11DeviceContext *context)
    {
        if (device != nullptr)
            m_d3d11_device.store(device, std::memory_order_release);
        if (context != nullptr)
            m_d3d11_context.store(context, std::memory_order_release);
    }

    void RenderBackend::set_d3d9(IDirect3DDevice9 *device)
    {
        if (device != nullptr)
            m_d3d9_device.store(device, std::memory_order_release);
    }

    void RenderBackend::set_swapchain(IDXGISwapChain *swapchain)
    {
        if (swapchain == nullptr)
            return;

        m_swapchain.store(swapchain, std::memory_order_release);

        // The window comes from the swapchain rather than being searched for: a game with more than
        // one window would otherwise have us hooking input on the wrong one.
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(swapchain->GetDesc(&desc)) && desc.OutputWindow != nullptr)
            m_hwnd.store(desc.OutputWindow, std::memory_order_release);
    }
}
