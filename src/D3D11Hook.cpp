#include "D3D11Hook.h"
#include "HookManager.h"
#include "IATHook.h"
#include "TextureManager.h"
#include "TextureToolkitUI.h"
#include "Config.h"
#include "OSDBanner.h"
#include "Logger.h"
#include <atomic>
#include <imgui.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <vector>
#include <thread>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{
    static WNDPROC g_orig_wndproc = nullptr;

    static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (TextureToolkitUI::is_visible())
        {
            g_inside_imgui_render = true;
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            g_inside_imgui_render = false;

            if (msg == WM_INPUT)
                return 0; // Block raw input from game

            if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))
            {
                return 0; // Block keyboard and mouse from reaching game when UI is open
            }
        }

        return CallWindowProc(g_orig_wndproc, hWnd, msg, wParam, lParam);
    }

    struct MappedResourceData
    {
        ID3D11Resource *resource = nullptr;
        UINT subresource = 0;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
    };

    static thread_local std::unordered_map<ID3D11Resource *, MappedResourceData> s_mapped_resources;
    thread_local bool D3D11Hook::s_inside_injection = false;
    std::atomic<uint64_t> D3D11Hook::s_present_count{0};

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

        HMODULE dxgi_module = GetModuleHandleA("dxgi.dll");
        if (dxgi_module == nullptr)
        {
            dxgi_module = LoadLibraryA("dxgi.dll");
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

        if (dxgi_module != nullptr)
        {
            void *pCreateDXGIFactory = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory"));
            if (pCreateDXGIFactory != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory, &Hooked_CreateDXGIFactory, reinterpret_cast<void **>(&m_orig_create_dxgi_factory));
                IATHook::hook_all_modules("dxgi.dll", "CreateDXGIFactory", &Hooked_CreateDXGIFactory, reinterpret_cast<void **>(&m_orig_create_dxgi_factory));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory API & IAT hooks installed successfully.");
            }

            // CreateDXGIFactory2 (DXGI 1.3) is what a modern game actually calls. Without it we
            // never see the factory that creates the real swapchain, and fall back to a throwaway
            // device+swapchain instead -- the Deus Ex: Mankind Divided symptom exactly.
            void *pCreateDXGIFactory2 = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory2"));
            if (pCreateDXGIFactory2 != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory2, &Hooked_CreateDXGIFactory2, reinterpret_cast<void **>(&m_orig_create_dxgi_factory2));
                IATHook::hook_all_modules("dxgi.dll", "CreateDXGIFactory2", &Hooked_CreateDXGIFactory2, reinterpret_cast<void **>(&m_orig_create_dxgi_factory2));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory2 API & IAT hooks installed successfully.");
            }

            void *pCreateDXGIFactory1 = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory1"));
            if (pCreateDXGIFactory1 != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory1, &Hooked_CreateDXGIFactory1, reinterpret_cast<void **>(&m_orig_create_dxgi_factory1));
                IATHook::hook_all_modules("dxgi.dll", "CreateDXGIFactory1", &Hooked_CreateDXGIFactory1, reinterpret_cast<void **>(&m_orig_create_dxgi_factory1));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory1 API & IAT hooks installed successfully.");
            }
        }

        // Start the swapchain-Present watchdog on its own thread. It does nothing unless a D3D11
        // game turns up whose swapchain we never hook the normal way, in which case it falls back
        // to a throwaway swapchain to hook the shared Present slot (see bootstrap_dxgi_present).
        // Deferred to a thread because creating a D3D11 device under the DllMain loader lock (this
        // init runs from DLL_PROCESS_ATTACH) can deadlock; a thread started during DllMain does not
        // run until the loader lock is released, which is what we want.
        if (m_orig_create_device_and_swapchain != nullptr)
            std::thread(&D3D11Hook::bootstrap_dxgi_present, this).detach();

        m_initialized = true;
        return true;
    }

    void D3D11Hook::bootstrap_dxgi_present()
    {
        // Gate: only fall back to a dummy device when the game is actually a D3D11 title whose
        // own swapchain we never managed to hook. This keeps the common cases free of any extra
        // device creation, which matters both for pure-D3D9 games (Bully, GTA IV never touch
        // D3D11, so no device is ever made here) and for multi-overlay stacks (ReShade, Special K,
        // Lossless Scaling) where an unnecessary startup device/swapchain risks ordering conflicts.
        //   - m_orig_present set        -> the game's own swapchain got hooked; nothing to do.
        //   - m_orig_create_texture2d   -> set by hook_device, i.e. the game created a D3D11 device.
        for (int i = 0; i < 600; ++i) // ~60s budget for the game to start rendering
        {
            if (m_orig_present != nullptr)
                return; // a real swapchain got hooked the normal way; no dummy needed
            if (m_orig_create_texture2d != nullptr)
            {
                Sleep(2000); // D3D11 game: give its own swapchain a moment to hook first
                break;
            }
            Sleep(100);
        }

        // Bail unless this is a D3D11 game still lacking a Present hook. A D3D9-only game never
        // sets m_orig_create_texture2d, so it leaves here without ever creating a device.
        if (m_orig_present != nullptr || m_orig_create_texture2d == nullptr)
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
        HRESULT hr = m_orig_create_device_and_swapchain(
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

    void D3D11Hook::hook_swapchain(IDXGISwapChain *swapchain)
    {
        if (swapchain == nullptr || m_orig_present != nullptr)
            return;

        void **sc_vtable = *reinterpret_cast<void ***>(swapchain);
        void *present_addr = sc_vtable[8]; // IDXGISwapChain::Present is index 8

        HookManager::get().create_hook(present_addr, &Hooked_Present, reinterpret_cast<void **>(&m_orig_present));
        Logger::get().info("[D3D11Hook] REAL GAME SWAPCHAIN INTERCEPTED! Present hook active.");
    }

    void D3D11Hook::hook_context(ID3D11DeviceContext *context)
    {
        if (context == nullptr || m_orig_ps_set_shader_resources != nullptr)
            return;

        void **ctx_vtable = *reinterpret_cast<void ***>(context);

        void *ps_set_srv_addr = ctx_vtable[8]; // PSSetShaderResources is index 8
        void *map_addr = ctx_vtable[14]; // Map is index 14
        void *unmap_addr = ctx_vtable[15]; // Unmap is index 15

        HookManager::get().create_hook(ps_set_srv_addr, &Hooked_PSSetShaderResources, reinterpret_cast<void **>(&m_orig_ps_set_shader_resources));
        HookManager::get().create_hook(ctx_vtable[25], &Hooked_VSSetShaderResources, reinterpret_cast<void **>(&m_orig_vs_set_shader_resources)); // VSSetShaderResources
        HookManager::get().create_hook(ctx_vtable[67], &Hooked_CSSetShaderResources, reinterpret_cast<void **>(&m_orig_cs_set_shader_resources)); // CSSetShaderResources
        HookManager::get().create_hook(map_addr, &Hooked_Map, reinterpret_cast<void **>(&m_orig_map));
        HookManager::get().create_hook(unmap_addr, &Hooked_Unmap, reinterpret_cast<void **>(&m_orig_unmap));

        Logger::get().info("[D3D11Hook] REAL GAME DEVICE CONTEXT INTERCEPTED! Pixel, vertex and compute shader binding, Map and Unmap hooks active.");
    }

    void D3D11Hook::hook_dxgi_factory(IDXGIFactory *factory)
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

        if (m_hwnd && g_orig_wndproc)
        {
            SetWindowLongPtr(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
            g_orig_wndproc = nullptr;
        }

        m_initialized = false;
    }

    void D3D11Hook::hook_device(ID3D11Device *device)
    {
        if (device == nullptr || m_orig_create_texture2d != nullptr)
            return;

        void **device_vtable = *reinterpret_cast<void ***>(device);
        void *create_tex2d_addr = device_vtable[5]; // ID3D11Device::CreateTexture2D is index 5

        HookManager::get().create_hook(create_tex2d_addr, &Hooked_CreateTexture2D, reinterpret_cast<void **>(&m_orig_create_texture2d));
        Logger::get().info("[D3D11Hook] REAL GAME DEVICE INTERCEPTED! CreateTexture2D hook active.");
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_CreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D)
    {
        // Skip tracking when we're inside injection (creating replacement textures)
        if (s_inside_injection)
            return get().m_orig_create_texture2d(device, pDesc, pInitialData, ppTexture2D);

        static int s_logged_creations = 0;
        if (pDesc != nullptr && s_logged_creations < 50)
        {
            s_logged_creations++;
            std::string has_init_data = (pInitialData != nullptr) ? "Yes" : "No";
            Logger::get().debug("[D3D11Hook] Hooked_CreateTexture2D: Width=" + std::to_string(pDesc->Width) + ", Height=" + std::to_string(pDesc->Height) + ", Format=" + std::to_string(static_cast<uint32_t>(pDesc->Format)) + ", InitialData=" + has_init_data + ", Usage=" + std::to_string(pDesc->Usage) + ", BindFlags=" + std::to_string(pDesc->BindFlags));
        }

        HRESULT hr = get().m_orig_create_texture2d(device, pDesc, pInitialData, ppTexture2D);

        if (SUCCEEDED(hr) && ppTexture2D != nullptr && *ppTexture2D != nullptr && pDesc != nullptr)
        {
            if (pInitialData != nullptr && pInitialData->pSysMem != nullptr && pDesc->MipLevels > 0)
            {
                // Only track static shader resource textures
                if (pDesc->Usage == D3D11_USAGE_DEFAULT || pDesc->Usage == D3D11_USAGE_IMMUTABLE)
                {
                    if (pDesc->BindFlags & D3D11_BIND_SHADER_RESOURCE)
                    {
                        if (s_logged_creations < 50)
                        {
                            Logger::get().debug("[D3D11Hook] Hooked_CreateTexture2D: Registering texture!");
                        }
                        TextureManager::get().register_unmap_texture11(
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

    void D3D11Hook::init_imgui(IDXGISwapChain *swapchain)
    {
        if (m_imgui_initialized || swapchain == nullptr)
            return;

        if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&m_device))))
            return;

        hook_device(m_device);
        m_device->GetImmediateContext(&m_context);

        DXGI_SWAP_CHAIN_DESC desc = {};
        swapchain->GetDesc(&desc);
        m_hwnd = desc.OutputWindow;

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
        ImGui_ImplDX11_Init(m_device, m_context);

        m_imgui_initialized = true;
        Logger::get().info("[D3D11Hook] Dear ImGui initialized natively for real game DirectX 11 device.");

        // Which surface the overlay bound to. If a game presents from more than one swapchain we
        // bind to the first one that presents, which may not be the one on screen; these lines
        // (plus the "different swapchain" warning below) are what identifies that case in a log.
        RECT cr = {};
        if (m_hwnd != nullptr)
            GetClientRect(m_hwnd, &cr);
        Logger::get().info("[D3D11Hook] Overlay bound to swapchain 0x" + std::to_string(reinterpret_cast<uintptr_t>(swapchain)) +
                           " hwnd 0x" + std::to_string(reinterpret_cast<uintptr_t>(m_hwnd)) +
                           " backbuffer " + std::to_string(desc.BufferDesc.Width) + "x" + std::to_string(desc.BufferDesc.Height) +
                           " client " + std::to_string(cr.right - cr.left) + "x" + std::to_string(cr.bottom - cr.top) +
                           " windowed=" + std::to_string(desc.Windowed ? 1 : 0) +
                           " swapeffect=" + std::to_string(static_cast<int>(desc.SwapEffect)));
    }

    void D3D11Hook::render_imgui(IDXGISwapChain *swapchain)
    {
        if (!m_imgui_initialized)
        {
            init_imgui(swapchain);
        }

        if (!m_imgui_initialized)
            return;

        uint32_t toggle_key = ConfigManager::get().get_config().hotkey;
        static bool s_key_was_down = false;
        bool key_is_down = (GetAsyncKeyState(toggle_key) & 0x8000) != 0;
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

        TextureManager::get().on_frame();

        // Nothing to draw. What this avoids is not the empty draw list, it is everything below:
        // fetching the back buffer and creating a render target view every single frame in order
        // to render nothing into it, which is a driver-side resource creation per frame for the
        // entire time the panel is closed -- which is nearly always.
        if (!TextureToolkitUI::is_visible() && !OSDBanner::get().is_active())
            return;

        g_inside_imgui_render = true;

        ImGuiIO &io = ImGui::GetIO();
        if (TextureToolkitUI::is_visible())
        {
            TextureToolkitUI::feed_overlay_mouse(m_hwnd);
        }
        else
        {
            io.MouseDrawCursor = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        TextureToolkitUI::draw_ui();

        ImGui::EndFrame();
        ImGui::Render();

        g_inside_imgui_render = false;

        // Save current DX11 Render Targets & Viewports
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
                // Fallback to nullptr desc if explicit desc fails
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

        // Restore original render targets & viewports
        m_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
        if (old_rtv != nullptr) old_rtv->Release();
        if (old_dsv != nullptr) old_dsv->Release();

        if (num_viewports > 0)
        {
            m_context->RSSetViewports(num_viewports, old_viewports);
        }
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
        {
            if (ppSwapChain != nullptr && *ppSwapChain != nullptr)
            {
                get().hook_swapchain(*ppSwapChain);
            }
            if (ppImmediateContext != nullptr && *ppImmediateContext != nullptr)
            {
                get().hook_context(*ppImmediateContext);
            }
            if (ppDevice != nullptr && *ppDevice != nullptr)
            {
                get().hook_device(*ppDevice);
            }
        }
        else
        {
            Logger::get().error("[D3D11Hook] D3D11CreateDeviceAndSwapChain failed with HRESULT: " + std::to_string(hr));
        }

        return hr;
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
        {
            if (ppImmediateContext != nullptr && *ppImmediateContext != nullptr)
            {
                get().hook_context(*ppImmediateContext);
            }
            if (ppDevice != nullptr && *ppDevice != nullptr)
            {
                get().hook_device(*ppDevice);
            }
        }
        else
        {
            Logger::get().error("[D3D11Hook] D3D11CreateDevice failed with HRESULT: " + std::to_string(hr));
        }

        return hr;
    }

    HRESULT WINAPI D3D11Hook::Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory)
    {
        Logger::get().info("[D3D11Hook] CreateDXGIFactory was called by the game!");
        HRESULT hr = E_FAIL;
        if (get().m_orig_create_dxgi_factory)
        {
            hr = get().m_orig_create_dxgi_factory(riid, ppFactory);
        }

        if (SUCCEEDED(hr) && ppFactory != nullptr && *ppFactory != nullptr)
        {
            get().hook_dxgi_factory(static_cast<IDXGIFactory *>(*ppFactory));
        }
        return hr;
    }

    HRESULT WINAPI D3D11Hook::Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory)
    {
        Logger::get().info("[D3D11Hook] CreateDXGIFactory1 was called by the game!");
        HRESULT hr = E_FAIL;
        if (get().m_orig_create_dxgi_factory1)
        {
            hr = get().m_orig_create_dxgi_factory1(riid, ppFactory);
        }

        if (SUCCEEDED(hr) && ppFactory != nullptr && *ppFactory != nullptr)
        {
            get().hook_dxgi_factory(static_cast<IDXGIFactory *>(*ppFactory));
        }
        return hr;
    }

    HRESULT WINAPI D3D11Hook::Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory)
    {
        Logger::get().info("[D3D11Hook] CreateDXGIFactory2 was called by the game!");
        HRESULT hr = E_FAIL;
        if (get().m_orig_create_dxgi_factory2)
        {
            hr = get().m_orig_create_dxgi_factory2(Flags, riid, ppFactory);
        }

        if (SUCCEEDED(hr) && ppFactory != nullptr && *ppFactory != nullptr)
        {
            get().hook_dxgi_factory(static_cast<IDXGIFactory *>(*ppFactory));
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_CreateSwapChain(IDXGIFactory *factory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain)
    {
        Logger::get().info("[D3D11Hook] IDXGIFactory::CreateSwapChain was called by the game!");
        HRESULT hr = get().m_orig_create_swapchain(factory, pDevice, pDesc, ppSwapChain);

        if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr)
        {
            get().hook_swapchain(*ppSwapChain);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain)
    {
        Logger::get().info("[D3D11Hook] IDXGIFactory2::CreateSwapChainForHwnd was called by the game!");
        HRESULT hr = get().m_orig_create_swapchain_for_hwnd(factory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);

        if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr)
        {
            get().hook_swapchain(*ppSwapChain); // IDXGISwapChain1 derives from IDXGISwapChain
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_Present(IDXGISwapChain *swapchain, UINT SyncInterval, UINT Flags)
    {
        // Note which swapchain is presenting, but do NOT skip rendering for it: a game can present
        // more than one, and drawing into every one that presents is what makes the overlay land on
        // the visible surface. (Binding to only the first one regressed Dead Rising 3.)
        if (get().m_imgui_initialized && swapchain != get().m_swapchain)
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                Logger::get().warn("[D3D11Hook] More than one swapchain is presenting (0x" +
                                   std::to_string(reinterpret_cast<uintptr_t>(swapchain)) + " and 0x" +
                                   std::to_string(reinterpret_cast<uintptr_t>(get().m_swapchain)) +
                                   "); the overlay draws into each of them.");
            }
        }

        s_present_count.fetch_add(1, std::memory_order_relaxed);
        get().m_swapchain = swapchain;
        get().render_imgui(swapchain);
        return get().m_orig_present(swapchain, SyncInterval, Flags);
    }

    void D3D11Hook::bind_shader_resources(SetShaderResources_t original, ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        if (original == nullptr)
            return;

        if (ppShaderResourceViews == nullptr || NumViews == 0)
        {
            original(context, StartSlot, NumViews, ppShaderResourceViews);
            return;
        }

        // This is a per-draw hot path. Reuse a thread-local buffer (immediate + any
        // deferred contexts share this hooked vtable slot, so keep it thread-local) to
        // avoid a heap allocation on every call, and only pass a rewritten array when a
        // replacement actually applied.
        static thread_local std::vector<ID3D11ShaderResourceView *> s_replaced;
        s_replaced.resize(NumViews);

        bool any_replaced = false;
        for (UINT i = 0; i < NumViews; ++i)
        {
            ID3D11ShaderResourceView *r = TextureManager::get().get_replacement_srv11(ppShaderResourceViews[i]);
            s_replaced[i] = r;
            if (r != ppShaderResourceViews[i])
                any_replaced = true;
        }

        original(context, StartSlot, NumViews,
            any_replaced ? s_replaced.data() : ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_PSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(get().m_orig_ps_set_shader_resources, context, StartSlot, NumViews, ppShaderResourceViews);
    }

    // A texture sampled by a vertex or compute shader never reached the pixel stage, so it was
    // invisible to the panel and could not be replaced at all. Terrain that displaces vertices from
    // a heightmap, and anything a compute pass reads, land here.
    void STDMETHODCALLTYPE D3D11Hook::Hooked_VSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(get().m_orig_vs_set_shader_resources, context, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_CSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(get().m_orig_cs_set_shader_resources, context, StartSlot, NumViews, ppShaderResourceViews);
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_Map(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource)
    {
        HRESULT hr = get().m_orig_map(context, pResource, Subresource, MapType, MapFlags, pMappedResource);

        // Skip our own staging Map during a dump/injection readback (see dump_resource11).
        if (SUCCEEDED(hr) && !s_inside_injection && Subresource == 0 && pMappedResource != nullptr && pMappedResource->pData != nullptr)
        {
            static int s_logged_maps = 0;
            if (s_logged_maps < 20)
            {
                s_logged_maps++;
                Logger::get().debug("[D3D11Hook] Hooked_Map: resource=0x" + std::to_string(reinterpret_cast<uintptr_t>(pResource)));
            }

            MappedResourceData data;
            data.resource = pResource;
            data.subresource = Subresource;
            data.mapped = *pMappedResource;

            s_mapped_resources[pResource] = data;
        }

        return hr;
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_Unmap(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource)
    {
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
                        Logger::get().debug("[D3D11Hook] Hooked_Unmap: Registering texture=0x" + std::to_string(reinterpret_cast<uintptr_t>(pResource)));
                    }

                    ID3D11Device *device = nullptr;
                    context->GetDevice(&device);

                    if (device != nullptr)
                    {
                        TextureManager::get().register_unmap_texture11(
                            device,
                            pResource,
                            data.mapped.pData,
                            desc.Width,
                            desc.Height,
                            desc.Format,
                            data.mapped.RowPitch
                        );
                        device->Release();
                    }
                }

                s_mapped_resources.erase(it);
            }
        }

        get().m_orig_unmap(context, pResource, Subresource);
    }
}
