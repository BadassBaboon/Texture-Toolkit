#include "D3D11Hook.h"
#include <d3d11_1.h>
#include "HookManager.h"
#include "IATHook.h"
#include "TextureManager.h"
#include "TextureToolkitUI.h"
#include "UITheme.h"
#include "Config.h"
#include "OSDBanner.h"
#include "Logger.h"
#include "HookTimings.h"
#include "PathUtil.h"
#include "ScopedFlag.h"
#include "Logo.h"
#include <atomic>
#include <imgui.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{
    static WNDPROC g_orig_wndproc = nullptr;

    static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (TextureToolkitUI::is_visible())
        {
            // Keys are polled (see feed_overlay_keyboard); handing them to ImGui here as well
            // would type them twice in a game that does deliver them as messages.
            LRESULT handled = 0;
            if (msg < WM_KEYFIRST || msg > WM_KEYLAST)
            {
                ScopedFlag reading_real_input(g_inside_imgui_render);
                handled = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            }

            // ImGui answers WM_SETCURSOR with its own cursor (none, while it draws the software
            // one). Passing it on as well let the game put its cursor straight back.
            if (msg == WM_SETCURSOR && handled != 0)
                return handled;

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

    // `resource` holds a reference for as long as its entry exists (taken in Hooked_Map, dropped
    // in Hooked_Unmap). The map is keyed on the raw pointer, so without that reference a resource
    // the game Released while still mapped could be destroyed, its address reused by an unrelated
    // resource, and that resource's Unmap would match the stale entry and hand a dead pData to
    // the hasher.
    struct MappedResourceData
    {
        ID3D11Resource *resource = nullptr;
        UINT subresource = 0;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
    };

    // Shared across threads rather than thread_local: a game may Map on one thread and Unmap on
    // another, and with a reference held per entry a per-thread map would leak that resource.
    static std::unordered_map<ID3D11Resource *, MappedResourceData> s_mapped_resources;
    static std::mutex s_mapped_mutex;
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
                IATHook::hook_game_exe("d3d11.dll", "D3D11CreateDeviceAndSwapChain", &Hooked_D3D11CreateDeviceAndSwapChain, reinterpret_cast<void **>(&m_orig_create_device_and_swapchain));
                Logger::get().info("[D3D11Hook] D3D11CreateDeviceAndSwapChain API & IAT hooks installed successfully.");
            }

            void *pD3D11CreateDevice = reinterpret_cast<void *>(GetProcAddress(d3d11_module, "D3D11CreateDevice"));
            if (pD3D11CreateDevice != nullptr)
            {
                HookManager::get().create_hook(pD3D11CreateDevice, &Hooked_D3D11CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
                IATHook::hook_game_exe("d3d11.dll", "D3D11CreateDevice", &Hooked_D3D11CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
                Logger::get().info("[D3D11Hook] D3D11CreateDevice API & IAT hooks installed successfully.");
            }
        }

        if (dxgi_module != nullptr)
        {
            void *pCreateDXGIFactory = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory"));
            if (pCreateDXGIFactory != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory, &Hooked_CreateDXGIFactory, reinterpret_cast<void **>(&m_orig_create_dxgi_factory));
                IATHook::hook_game_exe("dxgi.dll", "CreateDXGIFactory", &Hooked_CreateDXGIFactory, reinterpret_cast<void **>(&m_orig_create_dxgi_factory));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory API & IAT hooks installed successfully.");
            }

            // CreateDXGIFactory2 (DXGI 1.3) is what a modern game actually calls. Without it we
            // never see the factory that creates the real swapchain, and fall back to a throwaway
            // device+swapchain instead -- the Deus Ex: Mankind Divided symptom exactly.
            void *pCreateDXGIFactory2 = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory2"));
            if (pCreateDXGIFactory2 != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory2, &Hooked_CreateDXGIFactory2, reinterpret_cast<void **>(&m_orig_create_dxgi_factory2));
                IATHook::hook_game_exe("dxgi.dll", "CreateDXGIFactory2", &Hooked_CreateDXGIFactory2, reinterpret_cast<void **>(&m_orig_create_dxgi_factory2));
                Logger::get().info("[D3D11Hook] CreateDXGIFactory2 API & IAT hooks installed successfully.");
            }

            void *pCreateDXGIFactory1 = reinterpret_cast<void *>(GetProcAddress(dxgi_module, "CreateDXGIFactory1"));
            if (pCreateDXGIFactory1 != nullptr)
            {
                HookManager::get().create_hook(pCreateDXGIFactory1, &Hooked_CreateDXGIFactory1, reinterpret_cast<void **>(&m_orig_create_dxgi_factory1));
                IATHook::hook_game_exe("dxgi.dll", "CreateDXGIFactory1", &Hooked_CreateDXGIFactory1, reinterpret_cast<void **>(&m_orig_create_dxgi_factory1));
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


    // ---------------------------------------------------------------------------------------
    // One original per hooked implementation.
    //
    // A swapchain or factory was hooked through the first one the game created, on the assumption
    // that every later one shares its code. A game that makes a throwaway swapchain at startup and
    // presents from a different kind later, or that presents with Present1, then never ran a single
    // frame through our hook: The Sims 4 tracked 207 textures and drew no overlay. Every distinct
    // function is hooked now, and each call is forwarded to the original belonging to the object
    // it was made on. MinHook patches the function body and leaves the vtable slot pointing at the
    // original address, so the slot itself is the key.
    // ---------------------------------------------------------------------------------------
    typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
    typedef HRESULT(STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
    typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
    typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainForHwndFn)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

    template <typename Fn>
    struct SlotHooks
    {
        struct Entry
        {
            void *target;
            Fn original;
        };

        static constexpr int kMax = 16;
        Entry entries[kMax] = {};
        std::atomic<int> count{0};
        std::mutex add_mutex;

        // Hooks `target` with `detour` the first time it is seen; true if that happened now. The
        // entry is published before the hook is armed, so a call landing the instant the patch
        // goes live always finds its original.
        bool add(void *target, void *detour)
        {
            std::lock_guard<std::mutex> lock(add_mutex);
            const int n = count.load(std::memory_order_relaxed);
            for (int i = 0; i < n; ++i)
                if (entries[i].target == target)
                    return false;
            if (n >= kMax)
                return false;

            Fn original = nullptr;
            if (!HookManager::get().prepare_hook(target, detour, &original))
                return false;
            entries[n].target = target;
            entries[n].original = original;
            count.store(n + 1, std::memory_order_release);
            HookManager::get().enable_hook(target);
            return true;
        }

        Fn find(void *target) const
        {
            const int n = count.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
                if (entries[i].target == target)
                    return entries[i].original;
            return nullptr;
        }

        bool empty() const { return count.load(std::memory_order_acquire) == 0; }
    };

    // Device-context functions. An immediate context and a deferred one are different
    // implementations, so a game that records its draws on deferred contexts (on worker threads)
    // binds every texture through functions the immediate context's hooks never see.
    typedef void(STDMETHODCALLTYPE *SetSrvFn)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
    typedef HRESULT(STDMETHODCALLTYPE *MapFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
    typedef void(STDMETHODCALLTYPE *UnmapFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
    typedef void(STDMETHODCALLTYPE *CopyResourceFn)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
    typedef void(STDMETHODCALLTYPE *CopyRegionFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
    typedef void(STDMETHODCALLTYPE *UpdateSubresourceFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);

    static SlotHooks<SetSrvFn> s_ps_srv_hooks;
    static SlotHooks<SetSrvFn> s_vs_srv_hooks;
    static SlotHooks<SetSrvFn> s_cs_srv_hooks;
    static SlotHooks<MapFn> s_map_hooks;
    static SlotHooks<UnmapFn> s_unmap_hooks;
    static SlotHooks<CopyRegionFn> s_copy_region_hooks;
    static SlotHooks<CopyResourceFn> s_copy_resource_hooks;
    static SlotHooks<UpdateSubresourceFn> s_update_subresource_hooks;

    // ID3D11DeviceContext1's versions of the same two, with a flags argument on the end. A game
    // built against D3D11.1 can upload through these and never touch the originals.
    typedef void(STDMETHODCALLTYPE *CopyRegion1Fn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *, UINT);
    typedef void(STDMETHODCALLTYPE *UpdateSubresource1Fn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT, UINT);
    static SlotHooks<CopyRegion1Fn> s_copy_region1_hooks;
    static SlotHooks<UpdateSubresource1Fn> s_update_subresource1_hooks;
    constexpr int kSlotCopySubresourceRegion1 = 115;
    constexpr int kSlotUpdateSubresource1 = 116;

    // Verbose diagnostics: how the game moves pixels into the textures it draws. Reported from
    // Present a few times, then left alone.
    struct CopyDiag
    {
        std::atomic<uint64_t> copy_resource{0}, copy_region{0}, copy_region_whole{0}, copy_region1{0};
        std::atomic<uint64_t> tag_carried{0}, update{0}, update_texture{0}, update1{0};
    };
    static CopyDiag s_copy_diag;

    // ID3D11DeviceContext vtable slots.
    constexpr int kSlotPSSetShaderResources = 8;
    constexpr int kSlotMap = 14;
    constexpr int kSlotUnmap = 15;
    constexpr int kSlotVSSetShaderResources = 25;
    constexpr int kSlotCopySubresourceRegion = 46;
    constexpr int kSlotCopyResource = 47;
    constexpr int kSlotUpdateSubresource = 48;
    constexpr int kSlotCSSetShaderResources = 67;

    static SlotHooks<PresentFn> s_present_hooks;
    static SlotHooks<Present1Fn> s_present1_hooks;
    static SlotHooks<CreateSwapChainFn> s_create_swapchain_hooks;
    static SlotHooks<CreateSwapChainForHwndFn> s_create_for_hwnd_hooks;

    static void *vtable_slot(void *object, int index)
    {
        return (*reinterpret_cast<void ***>(object))[index];
    }

    // Present and Present1 can be layered inside the runtime, one calling the other. The overlay is
    // drawn once per frame, by whichever the game called, and nested calls only forward.
    static thread_local int t_present_depth = 0;

    void D3D11Hook::bootstrap_dxgi_present()
    {
        // Gate: only fall back to a dummy device when the game is actually a D3D11 title whose
        // own swapchain we never managed to hook. This keeps the common cases free of any extra
        // device creation, which matters both for pure-D3D9 games (Bully, GTA IV never touch
        // D3D11, so no device is ever made here) and for multi-overlay stacks (ReShade, Special K,
        // Lossless Scaling) where an unnecessary startup device/swapchain risks ordering conflicts.
        //   - a Present hook exists     -> the game's own swapchain got hooked; nothing to do.
        //   - m_orig_create_texture2d   -> set by hook_device, i.e. the game created a D3D11 device.
        for (int i = 0; i < 600; ++i) // ~60s budget for the game to start rendering
        {
            if (!s_present_hooks.empty())
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
        if (!s_present_hooks.empty() || m_orig_create_texture2d == nullptr)
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
        if (swapchain == nullptr)
            return;

        // IDXGISwapChain::Present is index 8. Hooked for every implementation seen, not just the
        // first swapchain's: see SlotHooks.
        void *present = vtable_slot(swapchain, 8);
        if (s_present_hooks.add(present, reinterpret_cast<void *>(&Hooked_Present)))
            Logger::get().info("[D3D11Hook] REAL GAME SWAPCHAIN INTERCEPTED! Present hook active (" + ptr_hex(present) + ").");

        // IDXGISwapChain1::Present1 is index 22. A DXGI 1.2 game can present with it and never call
        // Present at all, which leaves a Present-only hook waiting forever.
        IDXGISwapChain1 *swapchain1 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void **>(&swapchain1))) && swapchain1 != nullptr)
        {
            void *present1 = vtable_slot(swapchain1, 22);
            if (s_present1_hooks.add(present1, reinterpret_cast<void *>(&Hooked_Present1)))
                Logger::get().info("[D3D11Hook] Present1 hook active (" + ptr_hex(present1) + ").");
            swapchain1->Release();
        }
    }

    void D3D11Hook::hook_context(ID3D11DeviceContext *context, const char *kind)
    {
        if (context == nullptr)
            return;

        // Every implementation, not only the first context seen: see the context tables above.
        // PSSetShaderResources answers whether this implementation is new; the rest follow it.
        const bool fresh = s_ps_srv_hooks.add(vtable_slot(context, kSlotPSSetShaderResources),
                                              reinterpret_cast<void *>(&Hooked_PSSetShaderResources));
        s_vs_srv_hooks.add(vtable_slot(context, kSlotVSSetShaderResources), reinterpret_cast<void *>(&Hooked_VSSetShaderResources));
        s_cs_srv_hooks.add(vtable_slot(context, kSlotCSSetShaderResources), reinterpret_cast<void *>(&Hooked_CSSetShaderResources));
        s_map_hooks.add(vtable_slot(context, kSlotMap), reinterpret_cast<void *>(&Hooked_Map));
        s_unmap_hooks.add(vtable_slot(context, kSlotUnmap), reinterpret_cast<void *>(&Hooked_Unmap));
        s_copy_region_hooks.add(vtable_slot(context, kSlotCopySubresourceRegion), reinterpret_cast<void *>(&Hooked_CopySubresourceRegion));
        s_copy_resource_hooks.add(vtable_slot(context, kSlotCopyResource), reinterpret_cast<void *>(&Hooked_CopyResource));
        s_update_subresource_hooks.add(vtable_slot(context, kSlotUpdateSubresource), reinterpret_cast<void *>(&Hooked_UpdateSubresource));

        ID3D11DeviceContext1 *context1 = nullptr;
        if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&context1))) && context1 != nullptr)
        {
            s_copy_region1_hooks.add(vtable_slot(context1, kSlotCopySubresourceRegion1), reinterpret_cast<void *>(&Hooked_CopySubresourceRegion1));
            s_update_subresource1_hooks.add(vtable_slot(context1, kSlotUpdateSubresource1), reinterpret_cast<void *>(&Hooked_UpdateSubresource1));
            context1->Release();
        }

        if (fresh)
            Logger::get().info(std::string("[D3D11Hook] REAL GAME DEVICE CONTEXT INTERCEPTED (") + kind + ", " +
                               ptr_hex(vtable_slot(context, kSlotPSSetShaderResources)) +
                               ")! Shader resource binding, Map, Unmap, copy and UpdateSubresource hooks active.");
    }

    void D3D11Hook::hook_dxgi_factory(IDXGIFactory *factory)
    {
        if (factory == nullptr)
            return;

        // IDXGIFactory::CreateSwapChain is index 10. Every factory implementation is hooked: a game
        // can make a second factory through another entry point (CreateDXGIFactory2, say) whose
        // swapchains would otherwise never be seen.
        void *create_swapchain_addr = vtable_slot(factory, 10);
        if (s_create_swapchain_hooks.add(create_swapchain_addr, reinterpret_cast<void *>(&Hooked_CreateSwapChain)))
            Logger::get().info("[D3D11Hook] Intercepted IDXGIFactory::CreateSwapChain (VTable index 10, " + ptr_hex(create_swapchain_addr) + ").");

        // Flip-model games (DXGI 1.2+, e.g. Deus Ex: Mankind Divided) create their swapchain
        // through IDXGIFactory2::CreateSwapChainForHwnd and never touch CreateSwapChain, so we
        // must hook that too or the Present hook is never installed and the overlay never shows.
        IDXGIFactory2 *factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory2))) && factory2 != nullptr)
        {
            void *create_for_hwnd_addr = vtable_slot(factory2, 15); // IDXGIFactory2::CreateSwapChainForHwnd
            if (s_create_for_hwnd_hooks.add(create_for_hwnd_addr, reinterpret_cast<void *>(&Hooked_CreateSwapChainForHwnd)))
                Logger::get().info("[D3D11Hook] Intercepted IDXGIFactory2::CreateSwapChainForHwnd (VTable index 15, " + ptr_hex(create_for_hwnd_addr) + ").");
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
            Logo::release();
            { ScopedFlag own_draw(s_inside_injection); ImGui_ImplDX11_Shutdown(); }
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
        if (device == nullptr)
            return;

        if (m_orig_create_texture2d == nullptr)
        {
            void **device_vtable = *reinterpret_cast<void ***>(device);
            void *create_tex2d_addr = device_vtable[5]; // ID3D11Device::CreateTexture2D is index 5

            HookManager::get().create_hook(create_tex2d_addr, &Hooked_CreateTexture2D, reinterpret_cast<void **>(&m_orig_create_texture2d));
            Logger::get().info("[D3D11Hook] REAL GAME DEVICE INTERCEPTED! CreateTexture2D hook active.");
        }

        // The immediate context, for a game that never asked for it at creation time.
        ID3D11DeviceContext *immediate = nullptr;
        device->GetImmediateContext(&immediate);
        if (immediate != nullptr)
        {
            hook_context(immediate, "immediate");
            immediate->Release();
        }

        // The deferred implementation. The Sims 4 records its draws on deferred contexts and bound
        // not one texture through the immediate context's hooks in 45 seconds of play, so the scene
        // list stayed empty with 200 textures tracked. Rather than hook every CreateDeferredContext
        // variant (1, 2 and 3 exist), make one here, hook the functions it shares with every other
        // deferred context, and let it go. A single-threaded device refuses, and then has none.
        ID3D11DeviceContext *deferred = nullptr;
        if (SUCCEEDED(device->CreateDeferredContext(0, &deferred)) && deferred != nullptr)
        {
            hook_context(deferred, "deferred");
            deferred->Release();
        }
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
                        HookTimings::Scope timing(HookTimings::Site::D3D11Create);
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
        UI::init();

        const std::wstring exe_path = module_file_name(nullptr);
        std::filesystem::path game_dir = std::filesystem::path(exe_path).parent_path();
        std::filesystem::path imgui_ini = game_dir / ConfigManager::get().get_config().resource_root / "imgui.ini";

        static std::string ini_path_str = path_utf8(imgui_ini);
        ImGui::GetIO().IniFilename = ini_path_str.c_str();

        ImGui_ImplWin32_Init(m_hwnd);
        // Every ImGui backend call runs with s_inside_injection set: the panel's own textures (the
        // font atlas) and binds must not be tracked as the game's. They were, which put our font in
        // the texture list and let Blink in game, aimed at it, blink the whole panel. (The logo
        // guards its own creation.)
        { ScopedFlag own_draw(s_inside_injection); ImGui_ImplDX11_Init(m_device, m_context); }
        Logo::create_d3d11(m_device);

        m_imgui_initialized = true;
        Logger::get().info("[D3D11Hook] Dear ImGui initialized natively for real game DirectX 11 device.");

        // Which surface the overlay bound to. If a game presents from more than one swapchain we
        // bind to the first one that presents, which may not be the one on screen; these lines
        // (plus the "different swapchain" warning below) are what identifies that case in a log.
        RECT cr = {};
        if (m_hwnd != nullptr)
            GetClientRect(m_hwnd, &cr);
        Logger::get().info("[D3D11Hook] Overlay bound to swapchain " + ptr_hex(swapchain) +
                           " hwnd " + ptr_hex(m_hwnd) +
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
            // The cursor follows: feed_overlay_mouse takes it while the panel is open, and
            // release_overlay_mouse below gives the game its own back once it closes.
        }
        s_key_was_down = key_is_down;

        if (!TextureToolkitUI::is_visible())
        {
            TextureToolkitUI::release_overlay_mouse();
            // A closed panel leaves the game drawing normally: no blink, no pinned preview. Done
            // here because a closed panel may skip draw_ui altogether.
            TextureManager::get().set_highlight_target(0);
            TextureManager::get().set_preview_target(0);
        }

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

        { ScopedFlag own_draw(s_inside_injection); ImGui_ImplDX11_NewFrame(); }
        ImGui_ImplWin32_NewFrame();
        UI::apply_frame_scale(); // the Direct3D 11 backend honours FramebufferScale itself
        TextureToolkitUI::set_real_delta_time();
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
            { ScopedFlag own_draw(s_inside_injection); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); }
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
                get().hook_context(*ppImmediateContext, "immediate");
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
                get().hook_context(*ppImmediateContext, "immediate");
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
        const CreateSwapChainFn original = s_create_swapchain_hooks.find(vtable_slot(factory, 10));
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL; // hooked but unpublished: cannot happen, and must not guess
        HRESULT hr = original(factory, pDevice, pDesc, ppSwapChain);

        if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr)
        {
            get().hook_swapchain(*ppSwapChain);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain)
    {
        Logger::get().info("[D3D11Hook] IDXGIFactory2::CreateSwapChainForHwnd was called by the game!");
        const CreateSwapChainForHwndFn original = s_create_for_hwnd_hooks.find(vtable_slot(factory, 15));
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL;
        HRESULT hr = original(factory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);

        if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr)
        {
            get().hook_swapchain(*ppSwapChain); // IDXGISwapChain1 derives from IDXGISwapChain
        }
        return hr;
    }

    void D3D11Hook::present_overlay(IDXGISwapChain *swapchain)
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
                Logger::get().warn("[D3D11Hook] More than one swapchain is presenting (" + ptr_hex(swapchain) + " and " + ptr_hex(get().m_swapchain) +
                                   "); the overlay draws into each of them.");
            }
        }

        const uint64_t presents = s_present_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (HookTimings::enabled() && presents % 1200 == 0 && presents <= 1200 * 6)
        {
            const CopyDiag &c = s_copy_diag;
            Logger::get().debug("[Diag] Copies so far: CopyResource " + std::to_string(c.copy_resource.load()) +
                                ", CopySubresourceRegion " + std::to_string(c.copy_region.load()) + " (whole top level " +
                                std::to_string(c.copy_region_whole.load()) + "), CopySubresourceRegion1 " +
                                std::to_string(c.copy_region1.load()) + ", from a tagged texture " + std::to_string(c.tag_carried.load()) +
                                ". UpdateSubresource " + std::to_string(c.update.load()) + " (texture top level " +
                                std::to_string(c.update_texture.load()) + "), UpdateSubresource1 " + std::to_string(c.update1.load()) + ".");
        }
        get().m_swapchain = swapchain;
        HookTimings::frame();
        {
            HookTimings::Scope timing(HookTimings::Site::Overlay);
            get().render_imgui(swapchain);
        }
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_Present(IDXGISwapChain *swapchain, UINT SyncInterval, UINT Flags)
    {
        const PresentFn original = s_present_hooks.find(vtable_slot(swapchain, 8));
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL; // hooked but unpublished: cannot happen, and must not guess

        static std::atomic<bool> s_said{false};
        if (!s_said.exchange(true))
            Logger::get().info("[D3D11Hook] The game presents with IDXGISwapChain::Present.");

        if (++t_present_depth == 1)
            get().present_overlay(swapchain);
        const HRESULT hr = original(swapchain, SyncInterval, Flags);
        --t_present_depth;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_Present1(IDXGISwapChain1 *swapchain, UINT SyncInterval, UINT Flags,
                                                         const DXGI_PRESENT_PARAMETERS *pPresentParameters)
    {
        const Present1Fn original = s_present1_hooks.find(vtable_slot(swapchain, 22));
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL;

        static std::atomic<bool> s_said{false};
        if (!s_said.exchange(true))
            Logger::get().info("[D3D11Hook] The game presents with IDXGISwapChain1::Present1.");

        if (++t_present_depth == 1)
            get().present_overlay(swapchain);
        const HRESULT hr = original(swapchain, SyncInterval, Flags, pPresentParameters);
        --t_present_depth;
        return hr;
    }

    void D3D11Hook::bind_shader_resources(SetShaderResources_t original, ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        if (original == nullptr)
            return;

        // Our own drawing (the panel, its previews) is passed through untouched: it is not the
        // game's, and a preview of the selected texture must not blink with it.
        if (ppShaderResourceViews == nullptr || NumViews == 0 || s_inside_injection)
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
        {
            HookTimings::Scope timing(HookTimings::Site::D3D11Bind);
            for (UINT i = 0; i < NumViews; ++i)
            {
                ID3D11ShaderResourceView *r = TextureManager::get().get_replacement_srv11(ppShaderResourceViews[i]);
                s_replaced[i] = r;
                if (r != ppShaderResourceViews[i])
                    any_replaced = true;
            }
        }

        original(context, StartSlot, NumViews,
            any_replaced ? s_replaced.data() : ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_PSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(s_ps_srv_hooks.find(vtable_slot(context, kSlotPSSetShaderResources)), context, StartSlot, NumViews, ppShaderResourceViews);
    }

    // A texture sampled by a vertex or compute shader never reached the pixel stage, so it was
    // invisible to the panel and could not be replaced at all. Terrain that displaces vertices from
    // a heightmap, and anything a compute pass reads, land here.
    void STDMETHODCALLTYPE D3D11Hook::Hooked_VSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(s_vs_srv_hooks.find(vtable_slot(context, kSlotVSSetShaderResources)), context, StartSlot, NumViews, ppShaderResourceViews);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_CSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
    {
        bind_shader_resources(s_cs_srv_hooks.find(vtable_slot(context, kSlotCSSetShaderResources)), context, StartSlot, NumViews, ppShaderResourceViews);
    }

    // ---------------------------------------------------------------------------------------
    // Copies carry the content tag from source to destination
    //
    // The Sims 4 creates each texture twice: the one it draws (a default-usage shader resource) and
    // a staging one it fills through Map/Unmap, then copies across. Only the staging texture passed
    // through a hook we watched, so it was the one hashed and tagged, and staging textures can never
    // be bound: 211 tracked textures and not one of them in the scene. The tag now follows the copy.
    // Only a whole top-level copy carries it, since the top mip is what identifies a texture and a
    // partial copy is different content.
    // ---------------------------------------------------------------------------------------
    void STDMETHODCALLTYPE D3D11Hook::Hooked_CopyResource(ID3D11DeviceContext *context, ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource)
    {
        if (const CopyResourceFn original = s_copy_resource_hooks.find(vtable_slot(context, kSlotCopyResource)))
            original(context, pDstResource, pSrcResource);
        else
            return;
        s_copy_diag.copy_resource.fetch_add(1, std::memory_order_relaxed);
        if (!s_inside_injection && TextureManager::get().copy_tag11(pSrcResource, pDstResource))
            s_copy_diag.tag_carried.fetch_add(1, std::memory_order_relaxed);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_CopySubresourceRegion(ID3D11DeviceContext *context, ID3D11Resource *pDstResource, UINT DstSubresource,
                                                                   UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource,
                                                                   UINT SrcSubresource, const D3D11_BOX *pSrcBox)
    {
        if (const CopyRegionFn original = s_copy_region_hooks.find(vtable_slot(context, kSlotCopySubresourceRegion)))
            original(context, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
        else
            return;
        s_copy_diag.copy_region.fetch_add(1, std::memory_order_relaxed);
        carry_region_tag(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_CopySubresourceRegion1(ID3D11DeviceContext *context, ID3D11Resource *pDstResource, UINT DstSubresource,
                                                                    UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource,
                                                                    UINT SrcSubresource, const D3D11_BOX *pSrcBox, UINT CopyFlags)
    {
        if (const CopyRegion1Fn original = s_copy_region1_hooks.find(vtable_slot(context, kSlotCopySubresourceRegion1)))
            original(context, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
        else
            return;
        s_copy_diag.copy_region1.fetch_add(1, std::memory_order_relaxed);
        carry_region_tag(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
    }

    void D3D11Hook::carry_region_tag(ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ,
                                     ID3D11Resource *pSrcResource, UINT SrcSubresource, const D3D11_BOX *pSrcBox)
    {
        if (s_inside_injection || DstSubresource != 0 || SrcSubresource != 0 || DstX != 0 || DstY != 0 || DstZ != 0)
            return;

        // A box is still a whole copy when it covers all of the source's top level.
        if (pSrcBox != nullptr)
        {
            ID3D11Texture2D *src_tex = nullptr;
            if (pSrcResource == nullptr ||
                FAILED(pSrcResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&src_tex))) || src_tex == nullptr)
                return;
            D3D11_TEXTURE2D_DESC d = {};
            src_tex->GetDesc(&d);
            src_tex->Release();
            if (pSrcBox->left != 0 || pSrcBox->top != 0 || pSrcBox->right != d.Width || pSrcBox->bottom != d.Height)
                return;
        }

        s_copy_diag.copy_region_whole.fetch_add(1, std::memory_order_relaxed);
        if (TextureManager::get().copy_tag11(pSrcResource, pDstResource))
            s_copy_diag.tag_carried.fetch_add(1, std::memory_order_relaxed);
    }

    // Pixels written straight into a texture, with no staging copy in between. Registered exactly
    // as an Unmap would be: same hash, same tracking.
    void STDMETHODCALLTYPE D3D11Hook::Hooked_UpdateSubresource(ID3D11DeviceContext *context, ID3D11Resource *pDstResource, UINT DstSubresource,
                                                               const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch,
                                                               UINT SrcDepthPitch)
    {
        if (const UpdateSubresourceFn original = s_update_subresource_hooks.find(vtable_slot(context, kSlotUpdateSubresource)))
            original(context, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
        else
            return;
        s_copy_diag.update.fetch_add(1, std::memory_order_relaxed);
        register_update(context, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch);
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_UpdateSubresource1(ID3D11DeviceContext *context, ID3D11Resource *pDstResource, UINT DstSubresource,
                                                                const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch,
                                                                UINT SrcDepthPitch, UINT CopyFlags)
    {
        if (const UpdateSubresource1Fn original = s_update_subresource1_hooks.find(vtable_slot(context, kSlotUpdateSubresource1)))
            original(context, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);
        else
            return;
        s_copy_diag.update1.fetch_add(1, std::memory_order_relaxed);
        register_update(context, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch);
    }

    void D3D11Hook::register_update(ID3D11DeviceContext *context, ID3D11Resource *pDstResource, UINT DstSubresource,
                                    const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch)
    {
        if (s_inside_injection || DstSubresource != 0 || pDstBox != nullptr || pSrcData == nullptr || pDstResource == nullptr)
            return;

        // Buffers (constant buffers above all) arrive here far more often than textures do.
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        pDstResource->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            return;

        ID3D11Texture2D *tex = nullptr;
        if (FAILED(pDstResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
            return;
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        tex->Release();
        if ((d.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
            return;
        s_copy_diag.update_texture.fetch_add(1, std::memory_order_relaxed);

        ID3D11Device *device = nullptr;
        context->GetDevice(&device);
        if (device == nullptr)
            return;
        TextureManager::get().register_unmap_texture11(device, pDstResource, pSrcData, d.Width, d.Height, d.Format, SrcRowPitch);
        device->Release();
    }

    HRESULT STDMETHODCALLTYPE D3D11Hook::Hooked_Map(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource)
    {
        const MapFn original = s_map_hooks.find(vtable_slot(context, kSlotMap));
        if (original == nullptr)
            return E_FAIL; // hooked but unpublished: cannot happen, and must not guess
        HRESULT hr = original(context, pResource, Subresource, MapType, MapFlags, pMappedResource);

        // Skip our own staging Map during a dump/injection readback (see dump_resource11).
        if (SUCCEEDED(hr) && !s_inside_injection && Subresource == 0 && pMappedResource != nullptr && pMappedResource->pData != nullptr)
        {
            // Only 2D textures are ever hashed on Unmap; buffers (constant and dynamic vertex
            // buffers, mapped many times a frame) are not worth recording.
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            pResource->GetType(&dim);
            if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
                return hr;

            static int s_logged_maps = 0;
            if (s_logged_maps < 20)
            {
                s_logged_maps++;
                Logger::get().debug("[D3D11Hook] Hooked_Map: resource=" + ptr_hex(pResource));
            }

            MappedResourceData data;
            data.resource = pResource;
            data.subresource = Subresource;
            data.mapped = *pMappedResource;

            pResource->AddRef(); // released on the matching Unmap, or when superseded below

            // A resource can be mapped again without its previous mapping ever reaching our Unmap
            // (abandoned by a Release instead). Replace that entry and drop its reference.
            ID3D11Resource *superseded = nullptr;
            {
                std::lock_guard<std::mutex> lock(s_mapped_mutex);
                auto existing = s_mapped_resources.find(pResource);
                if (existing != s_mapped_resources.end())
                    superseded = existing->second.resource;
                s_mapped_resources[pResource] = data;
            }
            if (superseded != nullptr)
                superseded->Release();
        }

        return hr;
    }

    void STDMETHODCALLTYPE D3D11Hook::Hooked_Unmap(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource)
    {
        // The entry is always removed and its reference dropped, even while s_inside_injection is
        // set: the guard only decides whether this Unmap is hashed. Our own readback maps are never
        // recorded (see Hooked_Map), so a recorded entry here belongs to a real game mapping.
        ID3D11Resource *held = nullptr;

        if (Subresource == 0)
        {
            // Taken out under the lock, processed outside it: registering can create textures.
            MappedResourceData data;
            {
                std::lock_guard<std::mutex> lock(s_mapped_mutex);
                auto it = s_mapped_resources.find(pResource);
                if (it != s_mapped_resources.end())
                {
                    data = it->second;
                    s_mapped_resources.erase(it);
                    held = data.resource;
                }
            }

            if (held != nullptr)
            {
                if (!s_inside_injection && data.mapped.pData != nullptr)
                {
                    ID3D11Texture2D *tex = static_cast<ID3D11Texture2D *>(pResource);
                    D3D11_TEXTURE2D_DESC desc = {};
                    tex->GetDesc(&desc);

                    static int s_logged_unmaps = 0;
                    if (s_logged_unmaps < 20)
                    {
                        s_logged_unmaps++;
                        Logger::get().debug("[D3D11Hook] Hooked_Unmap: Registering texture=" + ptr_hex(pResource));
                    }

                    ID3D11Device *device = nullptr;
                    context->GetDevice(&device);

                    if (device != nullptr)
                    {
                        HookTimings::Scope timing(HookTimings::Site::D3D11Unmap);
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
            }
        }

        if (const UnmapFn original = s_unmap_hooks.find(vtable_slot(context, kSlotUnmap)))
            original(context, pResource, Subresource);

        // Dropped after the real Unmap so the resource is guaranteed alive across it.
        if (held != nullptr)
            held->Release();
    }
}
