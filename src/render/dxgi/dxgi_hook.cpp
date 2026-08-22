#include "render/dxgi/dxgi_hook.h"
#include "render/d3d11/d3d11_hook.h"
#include "render/render_backend.h"
#include "render/d3d11/d3d11_diagnostics.h"
#include "core/config.h"
#include "core/hook_manager.h"
#include "core/iat_hook.h"
#include "core/logger.h"
#include "texture/texture_manager.h"
#include "ui/overlay.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <thread>

namespace TextureToolkit
{
    // Every entry point below hands back a factory, a device or a swapchain, and every one of them
    // does the same thing with it. Only the call in the middle differs.
    namespace
    {
        HRESULT adopt_factory(HRESULT hr, void **out)
        {
            if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
                DXGIHook::get().hook_factory(static_cast<IDXGIFactory *>(*out));
            return hr;
        }

        template <typename T>
        HRESULT adopt_swapchain(HRESULT hr, T **out)
        {
            if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
                DXGIHook::get().hook_swapchain(*out);
            return hr;
        }
    }

    DXGIHook &DXGIHook::get()
    {
        static DXGIHook instance;
        return instance;
    }

    bool DXGIHook::init()
    {
        HMODULE dxgi_module = GetModuleHandleA("dxgi.dll");
        if (dxgi_module == nullptr)
            dxgi_module = LoadLibraryA("dxgi.dll");

        if (dxgi_module != nullptr)
        {
            void *pCreateDXGIFactory = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory"));
            if (pCreateDXGIFactory != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory, &DXGIHook::Hooked_CreateDXGIFactory, reinterpret_cast<void **>(&m_orig_create_dxgi_factory));
                IATHook::hook_all_modules("dxgi.dll", "CreateDXGIFactory", &DXGIHook::Hooked_CreateDXGIFactory, reinterpret_cast<void **>(&m_orig_create_dxgi_factory));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory API & IAT hooks installed successfully.");
            }

            void *pCreateDXGIFactory1 = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory1"));
            if (pCreateDXGIFactory1 != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory1, &DXGIHook::Hooked_CreateDXGIFactory1, reinterpret_cast<void **>(&m_orig_create_dxgi_factory1));
                IATHook::hook_all_modules("dxgi.dll", "CreateDXGIFactory1", &DXGIHook::Hooked_CreateDXGIFactory1, reinterpret_cast<void **>(&m_orig_create_dxgi_factory1));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory1 API & IAT hooks installed successfully.");
            }

            // What anything modern actually calls. Without it the game's swapchain is never seen
            // and the bootstrap fallback below fires, creating a throwaway device and swapchain
            // just to reach the Present slot -- which ReShade then builds a second runtime for.
            void *pCreateDXGIFactory2 = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory2"));
            if (pCreateDXGIFactory2 != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory2, &DXGIHook::Hooked_CreateDXGIFactory2, reinterpret_cast<void **>(&m_orig_create_dxgi_factory2));
                IATHook::hook_all_modules("dxgi.dll", "CreateDXGIFactory2", &DXGIHook::Hooked_CreateDXGIFactory2, reinterpret_cast<void **>(&m_orig_create_dxgi_factory2));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory2 API & IAT hooks installed successfully.");
            }
        }

        // Start the swapchain-Present watchdog on its own thread. It does nothing unless a D3D11
        // game turns up whose swapchain we never hook the normal way, in which case it falls back
        // to a throwaway swapchain to hook the shared Present slot (see bootstrap_present).
        // Deferred to a thread because creating a D3D11 device under the DllMain loader lock (this
        // init runs from DLL_PROCESS_ATTACH) can deadlock; a thread started during DllMain does not
        // run until the loader lock is released, which is what we want.
        // The thread outlives init and cannot be joined from DllMain, so it holds a reference on
        // this module for as long as it runs and drops it by exiting through
        // FreeLibraryAndExitThread. Without that, an explicit FreeLibrary would pull the code out
        // from under it while it sleeps.
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&DXGIHook::Hooked_Present), &self) == FALSE)
            return true;

        std::thread([this, self] {
            bootstrap_present();
            FreeLibraryAndExitThread(self, 0);
        }).detach();

        return true;
    }

    // --- Factories: three exported entry points, any of which a game may call ---

    void DXGIHook::hook_factory(IDXGIFactory *factory)
    {
        if (factory == nullptr || m_orig_create_swapchain != nullptr)
            return;

        void **factory_vtable = *reinterpret_cast<void ***>(factory);
        void *create_swapchain_addr = factory_vtable[10]; // IDXGIFactory::CreateSwapChain is index 10

        HookManager::get().create_hook(create_swapchain_addr, &Hooked_CreateSwapChain, reinterpret_cast<void **>(&m_orig_create_swapchain));
        Logger::get().info("[D3D11Hook] Intercepted IDXGIFactory::CreateSwapChain (VTable index 10).");

        // Flip-model games (DXGI 1.2+, e.g. Deus Ex: Mankind Divided) create their swapchain
        // through IDXGIFactory2::CreateSwapChainForHwnd and never touch CreateSwapChain, so we
        // must hook that too or the Present hook is never installed and the overlay never shows.
        IDXGIFactory2 *factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory2))) && factory2 != nullptr)
        {
            void **factory2_vtable = *reinterpret_cast<void ***>(factory2);
            void *create_for_hwnd_addr = factory2_vtable[15]; // IDXGIFactory2::CreateSwapChainForHwnd is index 15

            HookManager::get().create_hook(create_for_hwnd_addr, &Hooked_CreateSwapChainForHwnd, reinterpret_cast<void **>(&m_orig_create_swapchain_for_hwnd));
            Logger::get().info("[D3D11Hook] Intercepted IDXGIFactory2::CreateSwapChainForHwnd (VTable index 15).");
            factory2->Release();
        }
    }

    HRESULT WINAPI DXGIHook::Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory)
    {
        Logger::get().info("[D3D11Hook] CreateDXGIFactory intercepted.");
        const auto &orig = DXGIHook::get().m_orig_create_dxgi_factory;
        return adopt_factory(orig != nullptr ? orig(riid, ppFactory) : E_FAIL, ppFactory);
    }

    HRESULT WINAPI DXGIHook::Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory)
    {
        Logger::get().info("[D3D11Hook] CreateDXGIFactory1 intercepted.");
        const auto &orig = DXGIHook::get().m_orig_create_dxgi_factory1;
        return adopt_factory(orig != nullptr ? orig(riid, ppFactory) : E_FAIL, ppFactory);
    }

    HRESULT WINAPI DXGIHook::Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
    {
        Logger::get().info("[D3D11Hook] CreateDXGIFactory2 intercepted.");
        const auto &orig = DXGIHook::get().m_orig_create_dxgi_factory2;
        return adopt_factory(orig != nullptr ? orig(Flags, riid, ppFactory) : E_FAIL, ppFactory);
    }

    // --- Swapchains: however the game asks a factory for one ---

    void DXGIHook::hook_swapchain(IDXGISwapChain *swapchain)
    {
        if (!running())
            return;

        if (swapchain == nullptr || m_orig_present != nullptr)
            return;

        void **sc_vtable = *reinterpret_cast<void ***>(swapchain);
        void *present_addr = sc_vtable[8]; // IDXGISwapChain::Present is index 8

        HookManager::get().create_hook(present_addr, &Hooked_Present, reinterpret_cast<void **>(&m_orig_present));
        m_present_installed.store(true, std::memory_order_release);
        Logger::get().info("[D3D11Hook] REAL GAME SWAPCHAIN INTERCEPTED! Present hook active.");
    }

    HRESULT STDMETHODCALLTYPE DXGIHook::Hooked_CreateSwapChain(IDXGIFactory *factory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain)
    {
        Logger::get().info("[D3D11Hook] IDXGIFactory::CreateSwapChain intercepted.");
        return adopt_swapchain(DXGIHook::get().m_orig_create_swapchain(factory, pDevice, pDesc, ppSwapChain), ppSwapChain);
    }

    HRESULT STDMETHODCALLTYPE DXGIHook::Hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain)
    {
        Logger::get().info("[D3D11Hook] IDXGIFactory2::CreateSwapChainForHwnd intercepted.");
        return adopt_swapchain(DXGIHook::get().m_orig_create_swapchain_for_hwnd(factory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain), ppSwapChain);
    }

    void DXGIHook::bootstrap_present()
    {

        // Gate: only fall back to a dummy device when the game is actually a D3D11 title whose
        // own swapchain we never managed to hook. This keeps the common cases free of any extra
        // device creation, which matters both for pure-D3D9 games (Bully, GTA IV never touch
        // D3D11, so no device is ever made here) and for multi-overlay stacks (ReShade, Special K,
        // Lossless Scaling) where an unnecessary startup device/swapchain risks ordering conflicts.
        //   - present_installed() -> the game's own swapchain got hooked; nothing to do.
        //   - device_hooked()     -> the game created a D3D11 device, so it is a D3D11 title.
        // Every wait is interruptible: teardown must not have to sit through a sleep, and nothing
        // below may run once it has started.
        const auto wait = [this](int milliseconds) {
            for (int slept = 0; slept < milliseconds; slept += 50)
            {
                if (!running())
                    return false;
                Sleep(50);
            }
            return running();
        };

        for (int i = 0; i < 600; ++i) // ~60s budget for the game to start rendering
        {
            if (!running())
                return;
            if (present_installed())
                return; // a real swapchain got hooked the normal way; no dummy needed
            if (D3D11Hook::get().device_hooked())
            {
                if (!wait(2000)) // D3D11 game: give its own swapchain a moment to hook first
                    return;
                break;
            }
            if (!wait(100))
                return;
        }

        // Bail unless this is a D3D11 game still lacking a Present hook. A D3D9-only game never
        // hooks a D3D11 device, so it leaves here without ever creating one.
        if (!running() || present_installed() || !D3D11Hook::get().device_hooked())
            return;

        Logger::get().info("[D3D11Hook] No swapchain Present hooked yet; falling back to a bootstrap swapchain.");

        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"TTBootstrapWnd";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount = 1;
        scd.BufferDesc.Width = 16;
        scd.BufferDesc.Height = 16;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = (hwnd != nullptr) ? hwnd : GetDesktopWindow();
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain *sc = nullptr;
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        D3D_FEATURE_LEVEL fl = {};

        // Call the trampoline, not the hooked export, so this does not re-enter our own hook.
        HRESULT hr = D3D11Hook::get().original_create_device_and_swapchain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &scd, &sc, &dev, &fl, &ctx);

        if (SUCCEEDED(hr) && sc != nullptr)
        {
            hook_swapchain(sc); // hooks Present on the shared vtable -> catches the game's swapchain
            Logger::get().info("[D3D11Hook] Present hook installed via bootstrap swapchain.");
        }
        else
        {
            Logger::get().error("[D3D11Hook] Bootstrap swapchain creation failed (HRESULT " + std::to_string(hr) + "); overlay falls back to factory hooks.");
        }

        if (ctx != nullptr) ctx->Release();
        if (dev != nullptr) dev->Release();
        if (sc != nullptr) sc->Release();
        if (hwnd != nullptr) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }

    // --- Present: one vtable slot, shared by every swapchain in the process ---

    HRESULT STDMETHODCALLTYPE DXGIHook::Hooked_Present(IDXGISwapChain *swapchain, UINT SyncInterval, UINT Flags)
    {

        // Note which swapchain is presenting, but do NOT skip rendering for it: a game can present
        // more than one, and drawing into every one that presents is what makes the overlay land on
        // the visible surface. (Binding to only the first one regressed Dead Rising 3.)
        if (D3D11Hook::get().overlay_ready() && swapchain != get().m_swapchain)
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                Logger::get().warn("[D3D11Hook] More than one swapchain is presenting (0x" +
                                   Logger::fmt("%p", (swapchain)) + " and 0x" +
                                   Logger::fmt("%p", (get().m_swapchain)) +
                                   "); the overlay draws into each of them.");
            }
        }

        get().m_swapchain = swapchain;

#if TT_DIAGNOSTICS
        static LARGE_INTEGER s_qpc_freq = {};
        static LARGE_INTEGER s_prev_frame = {};
        static uint64_t s_overlay_prev = 0, s_hash_prev = 0, s_hash_bytes_prev = 0, s_builds_prev = 0;
        if (s_qpc_freq.QuadPart == 0)
            QueryPerformanceFrequency(&s_qpc_freq);

        LARGE_INTEGER frame_start = {};
        QueryPerformanceCounter(&frame_start);

        // Our hooks run BETWEEN presents, so each figure is the growth since the previous one.
        TextureManagerBase *tm_ptr = TextureManagerBase::active();
        if (tm_ptr == nullptr)
            return DXGIHook::get().m_orig_present(swapchain, SyncInterval, Flags);
        TextureManagerBase &tm = *tm_ptr;

        const uint64_t overlay_now = s_t_overlay.load(std::memory_order_relaxed);
        const uint64_t hash_now = tm.stat_hash_ticks.load(std::memory_order_relaxed);
        const uint64_t hash_bytes_now = tm.stat_hash_bytes.load(std::memory_order_relaxed);
        const uint64_t builds_now = tm.stat_builds.load(std::memory_order_relaxed);

        if (s_prev_frame.QuadPart != 0)
        {
            const double to_ms = 1000.0 / static_cast<double>(s_qpc_freq.QuadPart);
            const double frame_ms = static_cast<double>(frame_start.QuadPart - s_prev_frame.QuadPart) * to_ms;
            if (frame_ms > kSlowFrameMs)
            {
                // The periodic report resets these, so treat a decrease as "no data".
                const uint64_t d_overlay = (overlay_now >= s_overlay_prev) ? overlay_now - s_overlay_prev : 0;
                const uint64_t d_hash = (hash_now >= s_hash_prev) ? hash_now - s_hash_prev : 0;
                const uint64_t d_bytes = (hash_bytes_now >= s_hash_bytes_prev) ? hash_bytes_now - s_hash_bytes_prev : 0;
                const uint64_t d_builds = (builds_now >= s_builds_prev) ? builds_now - s_builds_prev : 0;

                report_slow_frame(frame_ms,
                                  static_cast<double>(d_overlay) * to_ms,
                                  static_cast<double>(d_hash) * to_ms,
                                  static_cast<double>(d_bytes) / (1024.0 * 1024.0),
                                  d_builds);
            }
        }

        s_overlay_prev = overlay_now;
        s_hash_prev = hash_now;
        s_hash_bytes_prev = hash_bytes_now;
        s_builds_prev = builds_now;
        s_prev_frame = frame_start;

        {
#if TT_DIAGNOSTICS
            QpcTimer t(s_t_overlay);
#endif
            D3D11Hook::get().render_imgui(swapchain);
        }
#else
        D3D11Hook::get().render_imgui(swapchain);
#endif

#if TT_DIAGNOSTICS
#if TT_DIAGNOSTICS
        QpcTimer t(s_t_present);
#endif
#endif
        return DXGIHook::get().m_orig_present(swapchain, SyncInterval, Flags);
    }
}
