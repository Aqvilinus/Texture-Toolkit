#include "render/d3d11/d3d11_hook.h"
#include "render/render_backend.h"
#include "render/dxgi/dxgi_hook.h"
#include "render/d3d11/d3d11_diagnostics.h"
#include "core/hook_manager.h"
#include "core/iat_hook.h"
#include "render/d3d11/d3d11_texture_manager.h"
#include "render/dxgi/dxgi_format.h"
#include "ui/overlay.h"
#include "core/config.h"
#include <DirectXTex.h>
#include "input/input_hook.h"
#include "ui/osd_banner.h"
#include "core/logger.h"
#include <imgui.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <vector>
#include <thread>
#include <cstdio>
#include <atomic>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{

    thread_local bool D3D11Hook::s_inside_injection = false;

    D3D11Hook &D3D11Hook::get()
    {
        static D3D11Hook instance;
        return instance;
    }

    D3D11Hook::~D3D11Hook()
    {
        shutdown();
    }

    bool D3D11Hook::init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;

        HMODULE d3d11_module = GetModuleHandleA("d3d11.dll");
        if (d3d11_module == nullptr)
        {
            d3d11_module = LoadLibraryA("d3d11.dll");
        }

        if (d3d11_module != nullptr)
        {
            void *pD3D11CreateDeviceAndSwapChain = reinterpret_cast<void *>(GetProcAddress(d3d11_module, "D3D11CreateDeviceAndSwapChain"));
            if (pD3D11CreateDeviceAndSwapChain != nullptr)
            {
                HookManager::get().create_hook(pD3D11CreateDeviceAndSwapChain, &Hooked_D3D11CreateDeviceAndSwapChain, reinterpret_cast<void **>(&m_orig_create_device_and_swapchain));
                IATHook::hook_all_modules("d3d11.dll", "D3D11CreateDeviceAndSwapChain", &Hooked_D3D11CreateDeviceAndSwapChain, reinterpret_cast<void **>(&m_orig_create_device_and_swapchain));
                Logger::get().info("[D3D11Hook] D3D11CreateDeviceAndSwapChain API & IAT hooks installed successfully.");
            }

            void *pD3D11CreateDevice = reinterpret_cast<void *>(GetProcAddress(d3d11_module, "D3D11CreateDevice"));
            if (pD3D11CreateDevice != nullptr)
            {
                HookManager::get().create_hook(pD3D11CreateDevice, &Hooked_D3D11CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
                IATHook::hook_all_modules("d3d11.dll", "D3D11CreateDevice", &Hooked_D3D11CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
                Logger::get().info("[D3D11Hook] D3D11CreateDevice API & IAT hooks installed successfully.");
            }
        }


        m_initialized = true;
        return true;
    }

    void D3D11Hook::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized)
            return;

        if (m_imgui_initialized)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            m_imgui_initialized = false;
        }

        InputHook::get().detach_from_window();

        m_initialized = false;
    }

    void D3D11Hook::hook_device(ID3D11Device *device)
    {
        if (device == nullptr || m_orig_create_texture2d != nullptr)
            return;

        // Not only in init_imgui: replacement building needs the device, and with the overlay
        // disabled init_imgui never runs. No AddRef -- the game owns it and outlives us.
        if (m_device == nullptr)
            m_device = device;
        RenderBackend::get().set_d3d11(device, nullptr);
        m_device_hooked.store(true, std::memory_order_release);

        void **device_vtable = *reinterpret_cast<void ***>(device);
        void *create_tex2d_addr = device_vtable[5]; // ID3D11Device::CreateTexture2D is index 5

        void *create_srv_addr = device_vtable[7]; // ID3D11Device::CreateShaderResourceView
        HookManager::get().create_hook(create_srv_addr, &Hooked_CreateShaderResourceView, reinterpret_cast<void **>(&m_orig_create_shader_resource_view));

        HookManager::get().create_hook(create_tex2d_addr, &Hooked_CreateTexture2D, reinterpret_cast<void **>(&m_orig_create_texture2d));
        Logger::get().info("[D3D11Hook] REAL GAME DEVICE INTERCEPTED! CreateTexture2D hook active.");
    }

    void D3D11Hook::hook_context(ID3D11DeviceContext *context)
    {
        if (context == nullptr || m_context_hooked)
            return;

        m_context_hooked = true;
        RenderBackend::get().set_d3d11(nullptr, context);

        void **ctx_vtable = *reinterpret_cast<void ***>(context);

        // Binding a view is where the game says which texture it is about to draw with; it does
        // not touch the texture's reference count, so there is nowhere else to learn it. The pixel
        // stage is where art is sampled, and the other two are here so a texture used only by a
        // vertex or compute shader still shows up.
        HookManager::get().create_hook(ctx_vtable[8], &Hooked_PSSetShaderResources,
                                       reinterpret_cast<void **>(&m_orig_ps_set_shader_resources));
        HookManager::get().create_hook(ctx_vtable[25], &Hooked_VSSetShaderResources,
                                       reinterpret_cast<void **>(&m_orig_vs_set_shader_resources));
        HookManager::get().create_hook(ctx_vtable[67], &Hooked_CSSetShaderResources,
                                       reinterpret_cast<void **>(&m_orig_cs_set_shader_resources));

        void *map_addr = ctx_vtable[14]; // Map is index 14
        void *unmap_addr = ctx_vtable[15]; // Unmap is index 15

        // Opt-out: every Unmap of a texture costs a CRC32 over the whole surface, which a game
        // that rewrites a dynamic texture each frame pays every frame -- 115-157 ms/frame in one
        // menu, against 3 ms for the entire rest of the tool. Not installing the detour, rather
        // than returning early from it, keeps even the trampoline off that path.
        if (ConfigManager::get().get_config().track_map_unmap)
        {
            HookManager::get().create_hook(map_addr, &Hooked_Map, reinterpret_cast<void **>(&m_orig_map));
            HookManager::get().create_hook(unmap_addr, &Hooked_Unmap, reinterpret_cast<void **>(&m_orig_unmap));
            Logger::get().info("[D3D11Hook] REAL GAME DEVICE CONTEXT INTERCEPTED! Map/Unmap hooks active.");
        }
        else
        {
            Logger::get().info("[D3D11Hook] REAL GAME DEVICE CONTEXT INTERCEPTED! Map/Unmap tracking off by config.");
        }
    }

    // --- Entry points: the exported functions that hand out a device ---

    namespace
    {
        void adopt_created(ID3D11Device **device, ID3D11DeviceContext **context, IDXGISwapChain **swapchain)
        {
            D3D11Hook &hook = D3D11Hook::get();
            if (swapchain != nullptr && *swapchain != nullptr)
                DXGIHook::get().hook_swapchain(*swapchain);
            if (context != nullptr && *context != nullptr)
                hook.hook_context(*context);
            if (device != nullptr && *device != nullptr)
                hook.hook_device(*device);
        }
    }

    HRESULT WINAPI D3D11Hook::Hooked_D3D11CreateDevice(
        IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
        const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
        ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
    {
        Logger::get().info("[D3D11Hook] D3D11CreateDevice was called by the game!");
        HRESULT hr = get().m_orig_create_device(
            pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
            ppDevice, pFeatureLevel, ppImmediateContext);

        if (SUCCEEDED(hr))
            adopt_created(ppDevice, ppImmediateContext, nullptr);
        else
            Logger::get().error("[D3D11Hook] D3D11CreateDevice failed: " + std::to_string(hr));

        return hr;
    }

    HRESULT WINAPI D3D11Hook::Hooked_D3D11CreateDeviceAndSwapChain(
        IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
        const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
        const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
        ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
    {
        Logger::get().info("[D3D11Hook] D3D11CreateDeviceAndSwapChain was called by the game!");
        HRESULT hr = get().m_orig_create_device_and_swapchain(
            pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
            pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

        if (SUCCEEDED(hr))
            adopt_created(ppDevice, ppImmediateContext, ppSwapChain);
        else
            Logger::get().error("[D3D11Hook] D3D11CreateDeviceAndSwapChain failed: " + std::to_string(hr));

        return hr;
    }

    HRESULT D3D11Hook::original_create_device_and_swapchain(
        IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
        const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
        const DXGI_SWAP_CHAIN_DESC *swap_desc, IDXGISwapChain **swapchain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context) const
    {
        return (m_orig_create_device_and_swapchain != nullptr)
                   ? m_orig_create_device_and_swapchain(adapter, type, software, flags, levels, level_count, sdk,
                                                        swap_desc, swapchain, device, level, context)
                   : E_FAIL;
    }

    // --- Textures: created, and the views the game builds onto them ---

    namespace
    {
        // Every view onto a texture we track, so the bind hook can recognise one by pointer alone
        // instead of walking back to the resource on every draw the way Special K does.
        void register_view(ID3D11Resource *resource, ID3D11ShaderResourceView **view)
        {
            // Skipped for our own previews and replacements: they are not what the game draws
            // with, and registering them would grow the table on every preview.
            if (view == nullptr || *view == nullptr || D3D11Hook::s_inside_injection)
                return;

            D3D11TextureManager &tm = D3D11TextureManager::get();
            const uint32_t hash = tm.resource_hash(resource);

            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            if (hash != 0)
            {
                (*view)->GetDesc(&vd);

                // Safe to take the manager lock here: our own views are created with the injection
                // guard set and never reach this line, so this is only ever the game creating one.
                if (vd.Format != DXGI_FORMAT_UNKNOWN)
                    tm.note_view_format(hash, static_cast<uint32_t>(vd.Format));
            }

            // A view onto an array, a cube or a volume is registered as untracked: a replacement is
            // a plain 2D texture, and sampling one through such a view is not what the shader
            // asked for. register_owned_view takes zero to mean exactly that.
            const bool substitutable = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
            tm.register_owned_view(*view, substitutable ? hash : 0);
        }
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_CreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D)
    {
        if (s_inside_injection)
            return get().m_orig_create_texture2d(device, pDesc, pInitialData, ppTexture2D);

        if (pDesc != nullptr && ppTexture2D != nullptr &&
            pInitialData != nullptr && pInitialData->pSysMem != nullptr && pDesc->MipLevels > 0 &&
            (pDesc->Usage == D3D11_USAGE_DEFAULT || pDesc->Usage == D3D11_USAGE_IMMUTABLE) &&
            (pDesc->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
        {
            // Const is cast away deliberately, as Special K does: the engine keeps this struct
            // and builds its shader resource view from it, so it has to learn the new format.
            if (ID3D11Texture2D *replaced = D3D11TextureManager::get().create_replacement_texture11(
                    device, *const_cast<D3D11_TEXTURE2D_DESC *>(pDesc), *pInitialData))
            {
                *ppTexture2D = replaced;
                return S_OK;
            }
        }

        HRESULT hr = get().m_orig_create_texture2d(device, pDesc, pInitialData, ppTexture2D);

        if (SUCCEEDED(hr) && ppTexture2D != nullptr && *ppTexture2D != nullptr && pDesc != nullptr)
        {
            if (pInitialData != nullptr && pInitialData->pSysMem != nullptr && pDesc->MipLevels > 0)
            {
                if (pDesc->Usage == D3D11_USAGE_DEFAULT || pDesc->Usage == D3D11_USAGE_IMMUTABLE)
                {
                    if (pDesc->BindFlags & D3D11_BIND_SHADER_RESOURCE)
                    {
                        D3D11TextureManager::get().register_texture11(
                            device,
                            *ppTexture2D,
                            pInitialData->pSysMem,
                            pDesc->Width,
                            pDesc->Height,
                            pDesc->Format,
                            pInitialData->SysMemPitch,
                            pInitialData,
                            pDesc->MipLevels
                        );
                    }
                }
            }
        }

        return hr;
    }

    // The game asks for a view using the format and mip count of the texture it believed it was
    // creating. For a replaced texture those no longer match, and D3D11 refuses the view outright
    // -- which the game reports as a failure and draws nothing. Correct the request instead of
    // constraining what a replacement may be.
    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_CreateShaderResourceView(ID3D11Device *device, ID3D11Resource *pResource,
                                                                        const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
                                                                        ID3D11ShaderResourceView **ppSRView)
    {
        if (pDesc != nullptr && pResource != nullptr && D3D11TextureManager::get().owns_resource(pResource))
        {
            ID3D11Texture2D *texture = nullptr;
            if (SUCCEEDED(pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture))) && texture != nullptr)
            {
                D3D11_TEXTURE2D_DESC tex_desc = {};
                texture->GetDesc(&tex_desc);
                texture->Release();

                const bool format_differs = pDesc->Format != tex_desc.Format;
                const bool mips_differ = pDesc->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
                                         pDesc->Texture2D.MipLevels != tex_desc.MipLevels;

                if (format_differs || mips_differ)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC fixed = *pDesc;

                    // Keep the game's sRGB intent: it picked that, the replacement's file did not.
                    fixed.Format = DirectX::IsSRGB(pDesc->Format) ? DirectX::MakeSRGB(tex_desc.Format)
                                                                  : tex_desc.Format;

                    if (mips_differ)
                    {
                        fixed.Texture2D.MipLevels = static_cast<UINT>(-1);
                        fixed.Texture2D.MostDetailedMip = 0;
                    }

                    const HRESULT hr = get().m_orig_create_shader_resource_view(device, pResource, &fixed, ppSRView);
                    if (SUCCEEDED(hr))
                    {
                        register_view(pResource, ppSRView);
                        return hr;
                    }
                }
            }
        }

        const HRESULT hr = get().m_orig_create_shader_resource_view(device, pResource, pDesc, ppSRView);
        if (SUCCEEDED(hr))
            register_view(pResource, ppSRView);

        return hr;
    }

    // Shared by all three stage hooks below: note what is on screen, and hand over a replacement
    // where one has been built for a texture the game already created.
    void D3D11Hook::bind_shader_resources(SetShaderResources_fn original, ID3D11DeviceContext *context, UINT StartSlot,
                                          UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        // Reused rather than a local array: a local one puts a stack cookie in the prologue of
        // every bind, and almost no bind substitutes anything. Per thread, since a game may bind
        // from more than one.
        static thread_local ID3D11ShaderResourceView *s_patched[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
        bool substituted = false;

        if (ppShaderResourceViews != nullptr && NumViews <= ARRAYSIZE(s_patched))
        {
            D3D11TextureManager &tm = D3D11TextureManager::get();

            // Both are the same answer for every view in this call, so they are asked once.
            const bool dumps_pending = tm.has_pending_dumps();
            const uint32_t preview_wanted = tm.preview_wanted();

            for (UINT i = 0; i < NumViews; ++i)
            {
                void *replacement = nullptr;
                const uint32_t hash = tm.note_referenced(ppShaderResourceViews[i], replacement);
                if (hash == 0)
                    continue;

                // A replacement built after the game made its texture rides on the view, since the
                // texture itself is the game's and usually immutable.
                if (replacement != nullptr)
                {
                    if (!substituted)
                    {
                        std::copy_n(ppShaderResourceViews, NumViews, s_patched);
                        substituted = true;
                    }
                    s_patched[i] = static_cast<ID3D11ShaderResourceView *>(replacement);
                }

                if (hash == preview_wanted)
                    tm.pin_preview_view(ppShaderResourceViews[i]);
                if (dumps_pending)
                    tm.note_dump_candidate(ppShaderResourceViews[i], hash);
            }
        }

        original(context, StartSlot, NumViews, substituted ? s_patched : ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_PSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(get().m_orig_ps_set_shader_resources, context, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_VSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(get().m_orig_vs_set_shader_resources, context, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_CSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(get().m_orig_cs_set_shader_resources, context, StartSlot, NumViews, ppShaderResourceViews);
    }

    // --- Uploads: a texture created empty and filled afterwards ---

    struct MappedResourceData
    {
        ID3D11Resource *resource = nullptr;
        UINT subresource = 0;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
    };

    static thread_local std::unordered_map<ID3D11Resource *, MappedResourceData> s_mapped_resources;

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_Map(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource)
    {
        HRESULT hr = get().m_orig_map(context, pResource, Subresource, MapType, MapFlags, pMappedResource);

#if TT_DIAGNOSTICS
        QpcTimer timer(s_t_mapunmap);
#endif

        // Skip our own staging Map during a dump/injection readback (see dump_resource11).
        if (SUCCEEDED(hr) && !s_inside_injection && Subresource == 0 && pMappedResource != nullptr && pMappedResource->pData != nullptr)
        {
            static int s_logged_maps = 0;
            if (s_logged_maps < 20)
            {
                s_logged_maps++;
                Logger::get().debug("[D3D11Hook] Hooked_Map: resource=0x" + Logger::fmt("%p", (pResource)));
            }

            MappedResourceData data;
            data.resource = pResource;
            data.subresource = Subresource;
            data.mapped = *pMappedResource;

#if TT_DIAGNOSTICS
            s_stat_map_records.fetch_add(1, std::memory_order_relaxed);
#endif
            s_mapped_resources[pResource] = data;
        }

        return hr;
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_Unmap(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource)
    {
#if TT_DIAGNOSTICS
        QpcTimer timer(s_t_mapunmap);
#endif

        if (!s_inside_injection && Subresource == 0)
        {
            auto it = s_mapped_resources.find(pResource);
            if (it != s_mapped_resources.end())
            {
                MappedResourceData &data = it->second;

                D3D11_RESOURCE_DIMENSION dim;
                pResource->GetType(&dim);

                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
                {
                    ID3D11Texture2D *tex = static_cast<ID3D11Texture2D *>(pResource);
                    D3D11_TEXTURE2D_DESC desc = {};
                    tex->GetDesc(&desc);

                    static int s_logged_unmaps = 0;
                    if (s_logged_unmaps < 20)
                    {
                        s_logged_unmaps++;
                        Logger::get().debug("[D3D11Hook] Hooked_Unmap: Registering texture=0x" + Logger::fmt("%p", (pResource)));
                    }

                    // What this texture used to be, so the replacement can be taken back off if the
                    // game has actually changed it. Only then: a texture rewritten every frame with
                    // the same content would otherwise drop and rebuild its replacement every frame.
                    D3D11TextureManager &tm = D3D11TextureManager::get();
                    const uint32_t previous = tm.resource_hash(pResource);

                    ID3D11Device *device = nullptr;
                    context->GetDevice(&device);

                    if (device != nullptr)
                    {
                        const uint32_t now = tm.register_texture11(
                            device,
                            pResource,
                            data.mapped.pData,
                            desc.Width,
                            desc.Height,
                            desc.Format,
                            data.mapped.RowPitch
                        );
                        device->Release();

                        // Registration has decided what the texture is now. If that is something
                        // else, the replacement standing in for the old content no longer describes
                        // anything, and registration has already queued one for the new content.
                        if (previous != 0 && previous != now)
                            tm.drop_override(previous);
                    }
                }

                s_mapped_resources.erase(it);
            }
        }

        get().m_orig_unmap(context, pResource, Subresource);
    }

    // --- The overlay, drawn from render_imgui ---

    void D3D11Hook::init_imgui(IDXGISwapChain *swapchain)
    {
        if (m_imgui_initialized || swapchain == nullptr)
            return;

        if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&m_device))))
            return;

        hook_device(m_device);
        m_device->GetImmediateContext(&m_context);

        // GetDevice and GetImmediateContext both hand back a reference. We keep the raw pointers
        // and drop those, on the same terms as everything else here: the game owns these objects
        // and we only touch them from inside calls it is making. Holding a reference instead would
        // keep a device alive past the game's own teardown.
        m_device->Release();
        m_context->Release();

        DXGI_SWAP_CHAIN_DESC desc = {};
        swapchain->GetDesc(&desc);
        m_hwnd = desc.OutputWindow;

        if (m_hwnd == nullptr)
        {
            m_hwnd = GetActiveWindow();
        }

        if (m_hwnd != nullptr)
        {
            InputHook::get().attach_to_window(m_hwnd);
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
        ImGui_ImplDX11_Init(m_device, m_context);

        m_imgui_initialized = true;
        Logger::get().info("[D3D11Hook] Dear ImGui initialized natively for real game DirectX 11 device.");

        // Which surface the overlay bound to. If a game presents from more than one swapchain we
        // bind to the first one that presents, which may not be the one on screen; these lines
        // (plus the "different swapchain" warning below) are what identifies that case in a log.
        RECT cr = {};
        if (m_hwnd != nullptr)
            GetClientRect(m_hwnd, &cr);
        Logger::get().info("[D3D11Hook] Overlay bound to swapchain 0x" + Logger::fmt("%p", (swapchain)) +
                           " hwnd 0x" + Logger::fmt("%p", (m_hwnd)) +
                           " backbuffer " + std::to_string(desc.BufferDesc.Width) + "x" + std::to_string(desc.BufferDesc.Height) +
                           " client " + std::to_string(cr.right - cr.left) + "x" + std::to_string(cr.bottom - cr.top) +
                           " windowed=" + std::to_string(desc.Windowed ? 1 : 0) +
                           " swapeffect=" + std::to_string(static_cast<int>(desc.SwapEffect)));
    }

    void D3D11Hook::render_imgui(IDXGISwapChain *swapchain)
    {
        if (!ConfigManager::get().get_config().enable_overlay)
        {
            D3D11TextureManager::get().on_frame();
#if TT_DIAGNOSTICS
            report_perf();
#endif
            return;
        }

        if (!m_imgui_initialized)
        {
            init_imgui(swapchain);
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
            // Cursor visibility is handled per-frame by feed_overlay_mouse (software cursor).
        }
        s_key_was_down = key_is_down;

        // Proof-of-life. If the overlay is invisible in game but these lines keep coming, we are
        // drawing into a surface that is not on screen rather than failing to run. Logged on a
        // few early frames so a short session still shows whether rendering is continuous.
        {
            static uint64_t s_frames = 0;
            ++s_frames;
            if (s_frames == 1 || s_frames == 10 || s_frames == 100 || s_frames == 1000)
            {
                char vk[16] = "";
                std::snprintf(vk, sizeof(vk), "0x%02X", toggle_key);
                Logger::get().info("[D3D11Hook] Overlay render heartbeat: frame " + std::to_string(s_frames) +
                                   ", hotkey vk=" + vk + ", foreground=" +
                                   std::to_string(GetForegroundWindow() == m_hwnd ? 1 : 0) +
                                   ", ui_visible=" + std::to_string(TextureToolkitUI::is_visible() ? 1 : 0));
            }
        }

        D3D11TextureManager::get().on_frame();
#if TT_DIAGNOSTICS
        report_perf();
#endif

        // Nothing to draw. What this avoids is not the empty draw list but everything below
        // it, which fetches the back buffer and creates a render target view every single frame
        // to render nothing into -- a driver-side resource creation per frame.
        if (!TextureToolkitUI::is_visible() && !OSDBanner::get().is_active())
            return;

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        TextureToolkitUI::draw_ui();

        ImGui::EndFrame();
        ImGui::Render();


        UINT num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT old_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        m_context->RSGetViewports(&num_viewports, old_viewports);

        ID3D11RenderTargetView *old_rtv = nullptr;
        ID3D11DepthStencilView *old_dsv = nullptr;
        m_context->OMGetRenderTargets(1, &old_rtv, &old_dsv);

        ID3D11RenderTargetView *rtv = nullptr;
        ID3D11Texture2D *back_buffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer))) && back_buffer != nullptr)
        {
            D3D11_TEXTURE2D_DESC bb_desc = {};
            back_buffer->GetDesc(&bb_desc);

            DXGI_FORMAT rtv_format = bb_desc.Format;
            if (rtv_format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
                rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (rtv_format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
                rtv_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            else if (rtv_format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
                rtv_format = DXGI_FORMAT_R10G10B10A2_UNORM;

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
            rtv_desc.Format = rtv_format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv_desc.Texture2D.MipSlice = 0;

            HRESULT hr_rtv = m_device->CreateRenderTargetView(back_buffer, &rtv_desc, &rtv);
            if (FAILED(hr_rtv))
            {
                m_device->CreateRenderTargetView(back_buffer, nullptr, &rtv);
            }
            back_buffer->Release();
        }

        if (rtv != nullptr)
        {
            m_context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            rtv->Release();
        }

        m_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
        if (old_rtv != nullptr) old_rtv->Release();
        if (old_dsv != nullptr) old_dsv->Release();

        if (num_viewports > 0)
        {
            m_context->RSSetViewports(num_viewports, old_viewports);
        }
    }
}
