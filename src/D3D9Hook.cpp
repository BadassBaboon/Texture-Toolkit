#include "D3D9Hook.h"
#include "ScopedFlag.h"
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <string>
#include "HookManager.h"
#include "IATHook.h"
#include "TextureManager.h"
#include "TextureToolkitUI.h"
#include "Config.h"
#include "Logger.h"
#include <atomic>
#include <imgui.h>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

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

    struct LockedTextureData
    {
        IDirect3DTexture9 *texture = nullptr;
        UINT width = 0;
        UINT height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        D3DLOCKED_RECT rect = {};
    };

    static thread_local std::unordered_map<IDirect3DTexture9 *, LockedTextureData> s_locked_textures;
    thread_local bool D3D9Hook::s_inside_injection = false;


    static std::string hex_string(DWORD v)
    {
        char buf[16] = {};
        snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(v));
        return buf;
    }

    // Written once, when the device is intercepted. A game whose art never reaches a lock is
    // almost always going through something: a d3d8-to-d3d9 wrapper, a D3DX redistributable, or
    // another overlay that got to the vtable first. Naming the graphics modules actually loaded
    // turns "no textures appear" from guesswork into a fact you can read off the log.
    static void log_graphics_modules()
    {
        HMODULE modules[512] = {};
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        {
            Logger::get().info("[D3D9Hook] Could not enumerate loaded modules.");
            return;
        }

        const size_t count = (std::min)(static_cast<size_t>(needed / sizeof(HMODULE)),
                                        sizeof(modules) / sizeof(modules[0]));

        std::string found;
        for (size_t i = 0; i < count; ++i)
        {
            char path[MAX_PATH] = {};
            if (GetModuleFileNameA(modules[i], path, MAX_PATH) == 0)
                continue;

            const char *slash = strrchr(path, '\\');
            std::string name = (slash != nullptr) ? (slash + 1) : path;
            const std::string full_path = path;

            std::string lower;
            lower.reserve(name.size());
            for (char c : name)
                lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));

            if (lower.rfind("d3d", 0) == 0 || lower.rfind("dxgi", 0) == 0 ||
                lower.rfind("ddraw", 0) == 0 || lower.rfind("dinput", 0) == 0 ||
                lower.find("reshade") != std::string::npos ||
                lower.find("dxvk") != std::string::npos)
            {
                if (!found.empty())
                    found += ", ";
                // Full path, because the same file name loaded twice is the signature of a
                // wrapper sitting in the game folder in front of the system DLL, and the bare
                // name cannot tell the two apart.
                found += full_path;
            }
        }

        Logger::get().info("[D3D9Hook] Graphics modules loaded: " + (found.empty() ? std::string("(none matched)") : found));
    }



    // One trampoline per hooked vtable.
    //
    // MinHook hands back a separate trampoline for each function it patches, and a trampoline is
    // only valid for the implementation it was made from. Hooking a second vtable and then calling
    // the first one's trampoline would run vtable A's LockRect against a vtable B texture, which is
    // the wrong function for the object entirely. So each hooked vtable keeps its own.
    //
    // MinHook patches the function body and leaves the vtable slot pointing at the original
    // address, so the slot itself is a stable key: read it off the object and look up the
    // trampoline that belongs to it. Entries are only ever appended, never changed, so a reader
    // needs no lock once it has seen the count.
    template <typename LockFn, typename UnlockFn>
    struct LockHookTable
    {
        struct Entry
        {
            void *lock_target;
            LockFn lock_tramp;
            UnlockFn unlock_tramp;
        };

        static constexpr int kMax = 16;
        Entry entries[kMax] = {};
        std::atomic<int> count{0};
        std::mutex add_mutex;

        // True if this vtable is new and was claimed by the caller, who must then fill it in.
        bool claim(void *lock_target)
        {
            std::lock_guard<std::mutex> lock(add_mutex);
            const int n = count.load(std::memory_order_relaxed);
            for (int i = 0; i < n; ++i)
            {
                if (entries[i].lock_target == lock_target)
                    return false;
            }
            return n < kMax;
        }

        void publish(void *lock_target, LockFn lock_tramp, UnlockFn unlock_tramp)
        {
            std::lock_guard<std::mutex> lock(add_mutex);
            const int n = count.load(std::memory_order_relaxed);
            if (n >= kMax)
                return;
            entries[n].lock_target = lock_target;
            entries[n].lock_tramp = lock_tramp;
            entries[n].unlock_tramp = unlock_tramp;
            count.store(n + 1, std::memory_order_release); // publishes the entry above
        }

        const Entry *find(void *lock_target) const
        {
            const int n = count.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
            {
                if (entries[i].lock_target == lock_target)
                    return &entries[i];
            }
            return nullptr;
        }
    };

    // Same signatures as the class typedefs, repeated here because those are private to D3D9Hook
    // and this table lives at file scope.
    typedef HRESULT(STDMETHODCALLTYPE *TexLock_fn)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
    typedef HRESULT(STDMETHODCALLTYPE *TexUnlock_fn)(IDirect3DTexture9 *, UINT);
    typedef HRESULT(STDMETHODCALLTYPE *SurfLock_fn)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
    typedef HRESULT(STDMETHODCALLTYPE *SurfUnlock_fn)(IDirect3DSurface9 *);

    static LockHookTable<TexLock_fn, TexUnlock_fn> s_texture_hooks;
    static LockHookTable<SurfLock_fn, SurfUnlock_fn> s_surface_hooks;

    // vtable slot 19 is IDirect3DTexture9::LockRect, 13 is IDirect3DSurface9::LockRect.
    static void *texture_lock_slot(IDirect3DTexture9 *texture)
    {
        return (*reinterpret_cast<void ***>(texture))[19];
    }

    static void *surface_lock_slot(IDirect3DSurface9 *surface)
    {
        return (*reinterpret_cast<void ***>(surface))[13];
    }

    // Unlock resolves the same way lock does, from the object's own vtable slot.
    static HRESULT unlock_original(IDirect3DTexture9 *texture, UINT Level)
    {
        const auto *hooks = s_texture_hooks.find(texture_lock_slot(texture));
        return (hooks != nullptr) ? hooks->unlock_tramp(texture, Level) : D3DERR_INVALIDCALL;
    }

    static HRESULT surface_unlock_original(IDirect3DSurface9 *surface)
    {
        const auto *hooks = s_surface_hooks.find(surface_lock_slot(surface));
        return (hooks != nullptr) ? hooks->unlock_tramp(surface) : D3DERR_INVALIDCALL;
    }

    // Diagnostic budget for the per-texture debug lines.
    //
    // These used to be a plain counter per call site, capped at 20 calls. One texture re-locked
    // every frame could spend the whole budget: in a Street Racing Syndicate log, a 640x480 video
    // surface locked 19 times in under a second used every LockRect line available, so nothing
    // that happened during the following 30 seconds appeared at all. The budget is spent per
    // DISTINCT texture now, so a texture that repeats costs exactly one line and cannot hide the
    // others. Verbose is checked first, which keeps all of this off the hot path by default.
    static constexpr size_t kMaxLoggedPerSite = 32;

    static bool should_log_texture(const char *site, const void *texture)
    {
        if (!Logger::get().debug_enabled())
            return false;

        static std::mutex mtx;
        static std::unordered_map<std::string, std::unordered_set<const void *>> seen;

        std::lock_guard<std::mutex> lock(mtx);
        auto &set = seen[site];
        if (set.size() >= kMaxLoggedPerSite)
            return false;
        return set.insert(texture).second;
    }

    static void log_texture_event(const char *tag, const char *site, const void *texture, UINT width, UINT height)
    {
        if (!should_log_texture(site, texture))
            return;

        Logger::get().debug(std::string("[") + tag + "] " + site + ": texture=0x" +
                            std::to_string(reinterpret_cast<uintptr_t>(texture)) + " " +
                            std::to_string(width) + "x" + std::to_string(height));
    }

    std::atomic<uint64_t> D3D9Hook::s_present_count{0};

    D3D9Hook &D3D9Hook::get()
    {
        static D3D9Hook instance;
        return instance;
    }

    D3D9Hook::~D3D9Hook()
    {
        shutdown();
    }

    bool D3D9Hook::init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return true;

        HMODULE d3d9_module = GetModuleHandleA("d3d9.dll");
        if (d3d9_module == nullptr)
        {
            d3d9_module = LoadLibraryA("d3d9.dll");
        }

        if (d3d9_module != nullptr)
        {
            void *pDirect3DCreate9 = reinterpret_cast<void *>(GetProcAddress(d3d9_module, "Direct3DCreate9"));
            if (pDirect3DCreate9 != nullptr)
            {
                HookManager::get().create_hook(pDirect3DCreate9, &Hooked_Direct3DCreate9, reinterpret_cast<void **>(&m_orig_direct3d_create9));
                IATHook::hook_all_modules("d3d9.dll", "Direct3DCreate9", &Hooked_Direct3DCreate9, reinterpret_cast<void **>(&m_orig_direct3d_create9));
                Logger::get().info("[D3D9Hook] Direct3DCreate9 API & IAT hooks installed successfully.");
            }

            void *pDirect3DCreate9Ex = reinterpret_cast<void *>(GetProcAddress(d3d9_module, "Direct3DCreate9Ex"));
            if (pDirect3DCreate9Ex != nullptr)
            {
                HookManager::get().create_hook(pDirect3DCreate9Ex, &Hooked_Direct3DCreate9Ex, reinterpret_cast<void **>(&m_orig_direct3d_create9_ex));
                IATHook::hook_all_modules("d3d9.dll", "Direct3DCreate9Ex", &Hooked_Direct3DCreate9Ex, reinterpret_cast<void **>(&m_orig_direct3d_create9_ex));
                Logger::get().info("[D3D9Hook] Direct3DCreate9Ex API & IAT hooks installed successfully.");
            }
        }

        m_initialized = true;
        return true;
    }

    IDirect3D9 *WINAPI D3D9Hook::Hooked_Direct3DCreate9(UINT SDKVersion)
    {
        IDirect3D9 *d3d9 = nullptr;
        if (get().m_orig_direct3d_create9 != nullptr)
        {
            d3d9 = get().m_orig_direct3d_create9(SDKVersion);
        }

        if (d3d9 != nullptr)
        {
            get().hook_d3d9_interface(d3d9);
        }
        return d3d9;
    }

    HRESULT WINAPI D3D9Hook::Hooked_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D)
    {
        HRESULT hr = E_FAIL;
        if (get().m_orig_direct3d_create9_ex != nullptr)
        {
            hr = get().m_orig_direct3d_create9_ex(SDKVersion, ppD3D);
        }

        if (SUCCEEDED(hr) && ppD3D != nullptr && *ppD3D != nullptr)
        {
            get().hook_d3d9_interface(*ppD3D);
        }
        return hr;
    }

    void D3D9Hook::hook_d3d9_interface(IDirect3D9 *d3d9)
    {
        if (d3d9 == nullptr || m_orig_create_device != nullptr)
            return;

        void **d3d9_vtable = *reinterpret_cast<void ***>(d3d9);
        void *create_device_addr = d3d9_vtable[16]; // IDirect3D9::CreateDevice is index 16

        HookManager::get().create_hook(create_device_addr, &Hooked_CreateDevice, reinterpret_cast<void **>(&m_orig_create_device));
        Logger::get().info("[D3D9Hook] Intercepted IDirect3D9::CreateDevice (VTable index 16).");
    }

    void D3D9Hook::hook_device_interface(IDirect3DDevice9 *device)
    {
        if (device == nullptr || m_device != nullptr)
            return;

        m_device = device;
        void **vtable = *reinterpret_cast<void ***>(device);

        void *present_addr = vtable[17];
        void *reset_addr = vtable[16];
        void *create_tex_addr = vtable[23];
        void *update_tex_addr = vtable[31];     // IDirect3DDevice9::UpdateTexture
        void *update_surface_addr = vtable[30]; // IDirect3DDevice9::UpdateSurface
        void *stretch_rect_addr = vtable[34];   // IDirect3DDevice9::StretchRect
        void *set_tex_addr = vtable[65];

        HookManager::get().create_hook(present_addr, &Hooked_Present, reinterpret_cast<void **>(&m_orig_present));
        HookManager::get().create_hook(reset_addr, &Hooked_Reset, reinterpret_cast<void **>(&m_orig_reset));
        HookManager::get().create_hook(create_tex_addr, &Hooked_CreateTexture, reinterpret_cast<void **>(&m_orig_create_texture));
        HookManager::get().create_hook(update_tex_addr, &Hooked_UpdateTexture, reinterpret_cast<void **>(&m_orig_update_texture));
        HookManager::get().create_hook(update_surface_addr, &Hooked_UpdateSurface, reinterpret_cast<void **>(&m_orig_update_surface));
        HookManager::get().create_hook(stretch_rect_addr, &Hooked_StretchRect, reinterpret_cast<void **>(&m_orig_stretch_rect));
        HookManager::get().create_hook(set_tex_addr, &Hooked_SetTexture, reinterpret_cast<void **>(&m_orig_set_texture));

        // D3D9Ex devices (e.g. GTA IV) present through PresentEx, which is a separate vtable
        // slot (121). Present (17) never fires for them, so hook PresentEx as well.
        IDirect3DDevice9Ex *device_ex = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&device_ex))) && device_ex != nullptr)
        {
            void **ex_vtable = *reinterpret_cast<void ***>(device_ex);
            void *present_ex_addr = ex_vtable[121]; // IDirect3DDevice9Ex::PresentEx
            HookManager::get().create_hook(present_ex_addr, &Hooked_PresentEx, reinterpret_cast<void **>(&m_orig_present_ex));
            Logger::get().info("[D3D9Hook] Device is D3D9Ex; hooked PresentEx (VTable index 121).");
            device_ex->Release();
        }

        // Some games (e.g. GTA IV) present through the swap chain, not the device, so
        // Present/PresentEx never fire. Hook the implicit swap chain's Present as well.
        IDirect3DSwapChain9 *swapchain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapchain)) && swapchain != nullptr)
        {
            void **sc_vtable = *reinterpret_cast<void ***>(swapchain);
            void *sc_present_addr = sc_vtable[3]; // IDirect3DSwapChain9::Present
            HookManager::get().create_hook(sc_present_addr, &Hooked_SwapChainPresent, reinterpret_cast<void **>(&m_orig_swapchain_present));
            Logger::get().info("[D3D9Hook] Hooked IDirect3DSwapChain9::Present (VTable index 3).");
            swapchain->Release();
        }

        Logger::get().info("[D3D9Hook] REAL GAME DEVICE INTERCEPTED! VTable hooks active on game IDirect3DDevice9.");

        log_graphics_modules();

        // A game that links D3DX statically has it loaded by now. One that loads it on demand is
        // picked up by the retry in Hooked_CreateTexture.
        hook_d3dx();
    }

    void D3D9Hook::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized)
            return;

        if (m_imgui_initialized)
        {
            ImGui_ImplDX9_Shutdown();
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

    void D3D9Hook::init_imgui(IDirect3DDevice9 *device)
    {
        if (m_imgui_initialized || device == nullptr)
            return;

        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow != nullptr)
        {
            m_hwnd = params.hFocusWindow;
        }

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
        ImGui_ImplDX9_Init(device);

        m_imgui_initialized = true;
        Logger::get().info("[D3D9Hook] Dear ImGui initialized natively for real game DirectX 9 device.");
    }

    void D3D9Hook::render_imgui(IDirect3DDevice9 *device)
    {
        if (!m_imgui_initialized)
        {
            init_imgui(device);
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

        TextureManager::get().on_frame();

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

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        TextureToolkitUI::draw_ui();

        ImGui::EndFrame();
        ImGui::Render();

        g_inside_imgui_render = false;
        
        // DX9 Present is outside of a scene, so begin one to draw ImGui. Only end the
        // scene if we actually began it (BeginScene fails if one is already open, in which
        // case a matching EndScene would wrongly close the game's scene).
        if (SUCCEEDED(device->BeginScene()))
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }
    }

    HRESULT WINAPI D3D9Hook::Hooked_CreateDevice(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnDeviceInterface)
    {
        Logger::get().info("[D3D9Hook] IDirect3D9::CreateDevice was called by the game!");
        
        HRESULT hr = get().m_orig_create_device(d3d9, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnDeviceInterface);

        if (SUCCEEDED(hr) && ppReturnDeviceInterface != nullptr && *ppReturnDeviceInterface != nullptr)
        {
            Logger::get().info("[D3D9Hook] CreateDevice succeeded. Hooking returned device...");
            get().hook_device_interface(*ppReturnDeviceInterface);
        }
        else
        {
            Logger::get().error("[D3D9Hook] CreateDevice failed with HRESULT: " + std::to_string(hr));
        }

        return hr;
    }

    // Guards against rendering the overlay twice when a game's device Present internally
    // routes through the swap chain Present (or vice versa). Only the outermost present
    // draws the overlay.
    static bool s_in_present = false;

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_Present(IDirect3DDevice9 *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
    {
        if (!s_in_present)
        {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; Logger::get().info("[D3D9Hook] First Present() call; overlay renders through Present."); }

            s_present_count.fetch_add(1, std::memory_order_relaxed);
            s_in_present = true;
            get().m_device = device;
            get().render_imgui(device);
            HRESULT hr = get().m_orig_present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
            s_in_present = false;
            return hr;
        }
        return get().m_orig_present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_PresentEx(IDirect3DDevice9Ex *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
    {
        if (!s_in_present)
        {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; Logger::get().info("[D3D9Hook] First PresentEx() call; overlay renders through PresentEx."); }

            s_present_count.fetch_add(1, std::memory_order_relaxed);
            s_in_present = true;
            get().m_device = device;
            get().render_imgui(device);
            HRESULT hr = get().m_orig_present_ex(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
            s_in_present = false;
            return hr;
        }
        return get().m_orig_present_ex(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SwapChainPresent(IDirect3DSwapChain9 *swapchain, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
    {
        if (!s_in_present && get().m_device != nullptr)
        {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; Logger::get().info("[D3D9Hook] First SwapChain Present() call; overlay renders through the swap chain."); }

            s_present_count.fetch_add(1, std::memory_order_relaxed);
            s_in_present = true;
            get().render_imgui(get().m_device);
            HRESULT hr = get().m_orig_swapchain_present(swapchain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
            s_in_present = false;
            return hr;
        }
        return get().m_orig_swapchain_present(swapchain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_Reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters)
    {
        if (get().m_imgui_initialized)
        {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        HRESULT hr = get().m_orig_reset(device, pPresentationParameters);

        if (SUCCEEDED(hr) && get().m_imgui_initialized)
        {
            ImGui_ImplDX9_CreateDeviceObjects();
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_CreateTexture(IDirect3DDevice9 *device, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
    {
        // A game can load d3dx9 on demand, after the device exists. Retry a bounded number of
        // times from here, which runs when art is being created rather than on every draw.
        if (!get().m_d3dx_hooked)
        {
            static std::atomic<int> s_d3dx_attempts{0};
            if (s_d3dx_attempts.fetch_add(1, std::memory_order_relaxed) < 64)
                get().hook_d3dx();
        }

        // Skip hooking new vtables when we're inside injection (creating replacement textures)
        if (s_inside_injection)
            return get().m_orig_create_texture(device, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);

        if (Logger::get().debug_enabled())
        {
            const char *pool_name = "?";
            switch (Pool)
            {
            case D3DPOOL_DEFAULT:   pool_name = "DEFAULT";   break;
            case D3DPOOL_MANAGED:   pool_name = "MANAGED";   break;
            case D3DPOOL_SYSTEMMEM: pool_name = "SYSTEMMEM"; break;
            case D3DPOOL_SCRATCH:   pool_name = "SCRATCH";   break;
            default: break;
            }

            Logger::get().debug("[D3D9Hook] CreateTexture: " + std::to_string(Width) + "x" + std::to_string(Height) +
                                " fmt=" + std::to_string(static_cast<uint32_t>(Format)) +
                                " levels=" + std::to_string(Levels) +
                                " usage=0x" + hex_string(Usage) +
                                " pool=" + pool_name);
        }
        HRESULT hr = get().m_orig_create_texture(device, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);

        if (SUCCEEDED(hr) && ppTexture != nullptr && *ppTexture != nullptr)
        {
            IDirect3DTexture9 *tex = *ppTexture;
            void **vtable = *reinterpret_cast<void ***>(tex);

            // Hook every DISTINCT vtable, not only the first texture's.
            //
            // We used to hook the first created texture and assume every other texture shared its
            // vtable. That holds in most games and is why this went unnoticed. It is not guaranteed:
            // a wrapper, a debug runtime or a differing implementation per pool can hand out a
            // second vtable, and textures created through it were then never seen locked at all --
            // created, bound, drawn, and completely absent from the panel. Street Racing Syndicate
            // creates 1446 MANAGED textures with no usage flags, which can only be filled by being
            // locked, and we observed four locks.
            //
            // Tracking the addresses we have already hooked makes this self-correcting: a new
            // vtable is hooked the moment a texture using it appears, and the log names it.
            if (s_texture_hooks.claim(vtable[19]))
            {
                LockRect_t lock_tramp = nullptr;
                UnlockRect_t unlock_tramp = nullptr;

                // Both trampolines are obtained and published BEFORE either hook is armed, so a
                // lock arriving the instant the patch lands always finds something to forward to.
                const bool ok_lock = HookManager::get().prepare_hook(vtable[19], &Hooked_LockRect, &lock_tramp);
                const bool ok_unlock = HookManager::get().prepare_hook(vtable[20], &Hooked_UnlockRect, &unlock_tramp);

                if (ok_lock && ok_unlock)
                {
                    s_texture_hooks.publish(vtable[19], lock_tramp, unlock_tramp);
                    HookManager::get().enable_hook(vtable[19]);
                    HookManager::get().enable_hook(vtable[20]);
                }

                Logger::get().info(std::string("[D3D9Hook] Hooked texture LockRect/UnlockRect on vtable 0x") +
                                   hex_string(static_cast<DWORD>(reinterpret_cast<uintptr_t>(vtable))) +
                                   " (pool=" + ((Pool == D3DPOOL_MANAGED) ? "MANAGED" : (Pool == D3DPOOL_DEFAULT) ? "DEFAULT" : "other") +
                                   ", lock=" + (ok_lock ? "ok" : "FAILED") +
                                   ", unlock=" + (ok_unlock ? "ok" : "FAILED") + ")");
            }

            IDirect3DSurface9 *pSurface = nullptr;
            if (SUCCEEDED(tex->GetSurfaceLevel(0, &pSurface)) && pSurface != nullptr)
            {
                void **surface_vtable = *reinterpret_cast<void ***>(pSurface);

                if (s_surface_hooks.claim(surface_vtable[13]))
                {
                    SurfaceLockRect_t lock_tramp = nullptr;
                    SurfaceUnlockRect_t unlock_tramp = nullptr;

                    const bool ok_lock = HookManager::get().prepare_hook(surface_vtable[13], &Hooked_SurfaceLockRect, &lock_tramp);
                    const bool ok_unlock = HookManager::get().prepare_hook(surface_vtable[14], &Hooked_SurfaceUnlockRect, &unlock_tramp);

                    if (ok_lock && ok_unlock)
                    {
                        s_surface_hooks.publish(surface_vtable[13], lock_tramp, unlock_tramp);
                        HookManager::get().enable_hook(surface_vtable[13]);
                        HookManager::get().enable_hook(surface_vtable[14]);
                    }

                    Logger::get().info(std::string("[D3D9Hook] Hooked surface LockRect/UnlockRect on vtable 0x") +
                                       hex_string(static_cast<DWORD>(reinterpret_cast<uintptr_t>(surface_vtable))) +
                                       " (lock=" + (ok_lock ? "ok" : "FAILED") +
                                       ", unlock=" + (ok_unlock ? "ok" : "FAILED") + ")");
                }

                pSurface->Release();
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_LockRect(IDirect3DTexture9 *texture, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
    {
        const auto *hooks = s_texture_hooks.find(texture_lock_slot(texture));
        if (hooks == nullptr)
            return D3DERR_INVALIDCALL; // hooked but unpublished: cannot happen, and must not guess

        HRESULT hr = hooks->lock_tramp(texture, Level, pLockedRect, pRect, Flags);

        // Skip tracking when we're inside injection (locking replacement textures)
        if (s_inside_injection)
            return hr;

        if (SUCCEEDED(hr) && Level == 0 && pLockedRect != nullptr && pLockedRect->pBits != nullptr)
        {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(texture->GetLevelDesc(0, &desc)))
            {
                log_texture_event("D3D9Hook", (pRect != nullptr) ? "LockRect (sub-rect)" : "LockRect",
                                  texture, desc.Width, desc.Height);

                // Ignore partial sub-rect locks (cannot safely hash/dump whole texture)
                if (pRect != nullptr)
                {
                    UINT rect_w = pRect->right - pRect->left;
                    UINT rect_h = pRect->bottom - pRect->top;
                    if (rect_w != desc.Width || rect_h != desc.Height)
                    {
                        return hr;
                    }
                }

                // Ignore dynamic textures (UI, fonts, frequently updated textures)
                if ((desc.Usage & D3DUSAGE_DYNAMIC) != 0)
                {
                    return hr;
                }

                LockedTextureData data;
                data.texture = texture;
                data.width = desc.Width;
                data.height = desc.Height;
                data.format = desc.Format;
                data.rect = *pLockedRect;

                s_locked_textures[texture] = data;
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_UnlockRect(IDirect3DTexture9 *texture, UINT Level)
    {
        // Skip processing when we're inside injection (unlocking replacement textures)
        if (s_inside_injection)
            return unlock_original(texture, Level);

        if (Level == 0)
        {
            auto it = s_locked_textures.find(texture);
            if (it != s_locked_textures.end())
            {
                LockedTextureData &data = it->second;
                log_texture_event("D3D9Hook", "UnlockRect registering", texture, data.width, data.height);

                // Pitch is signed here and unsigned from there on, so a negative one would become
                // an enormous row length instead of a refused lock.
                if (data.rect.Pitch > 0)
                {
                    TextureManager::get().register_unmap_texture9(
                        get().m_device,
                        texture,
                        data.rect.pBits,
                        data.width,
                        data.height,
                        data.format,
                        static_cast<UINT>(data.rect.Pitch)
                    );
                }

                s_locked_textures.erase(it);
            }
        }

        return unlock_original(texture, Level);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage, IDirect3DBaseTexture9 *pTexture)
    {
        if (pTexture != nullptr && should_log_texture("SetTexture", pTexture))
        {
            Logger::get().debug("[D3D9Hook] SetTexture: Stage=" + std::to_string(Stage) + " pTexture=0x" + std::to_string(reinterpret_cast<uintptr_t>(pTexture)));
        }

        IDirect3DBaseTexture9 *pReplacement = TextureManager::get().get_replacement_texture9(pTexture);
        return get().m_orig_set_texture(device, Stage, pReplacement);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_UpdateTexture(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
    {
        HRESULT hr = get().m_orig_update_texture(device, pSourceTexture, pDestinationTexture);

        // Games commonly load art into a SYSTEMMEM texture (which we hash and tag on unlock)
        // and copy it into the DEFAULT texture that is actually bound. Carry the hash tag
        // from source to destination so the bound texture is tracked, previewed and injected.
        if (SUCCEEDED(hr) && !s_inside_injection)
        {
            log_texture_event("D3D9Hook", "UpdateTexture", pDestinationTexture, 0, 0);
            TextureManager::get().copy_tag9(pSourceTexture, pDestinationTexture);
        }

        return hr;
    }



    // Reads back the top level of a texture that something else has already filled, and registers
    // it exactly as an unlock would. D3DX creates and populates a texture without the game ever
    // touching LockRect, so this is the only moment its pixels are visible to us.
    void D3D9Hook::register_loaded_texture(IDirect3DTexture9 *texture, const char *origin)
    {
        if (texture == nullptr || get().m_device == nullptr)
            return;

        D3DSURFACE_DESC desc = {};
        if (FAILED(texture->GetLevelDesc(0, &desc)))
            return;

        // A DEFAULT-pool texture cannot be locked for reading. D3DX creates MANAGED unless the
        // caller asked otherwise, so this covers the ordinary case and quietly skips the rest.
        if (desc.Pool == D3DPOOL_DEFAULT)
            return;

        // Our own LockRect/UnlockRect hooks must not treat this read-back as a game upload.
        ScopedFlag guard(s_inside_injection);

        D3DLOCKED_RECT rect = {};
        if (FAILED(texture->LockRect(0, &rect, nullptr, D3DLOCK_READONLY)))
            return;

        if (rect.pBits != nullptr && rect.Pitch > 0)
        {
            log_texture_event("D3D9Hook", origin, texture, desc.Width, desc.Height);
            TextureManager::get().register_unmap_texture9(
                get().m_device, texture, rect.pBits, desc.Width, desc.Height, desc.Format,
                static_cast<UINT>(rect.Pitch));
        }

        texture->UnlockRect(0);
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileInMemoryEx(IDirect3DDevice9 *device, const void *src, UINT size, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, void *palette, IDirect3DTexture9 **ppTexture)
    {
        HRESULT hr = get().m_orig_d3dx_from_memory_ex(device, src, size, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, palette, ppTexture);
        if (SUCCEEDED(hr) && ppTexture != nullptr && !s_inside_injection)
            register_loaded_texture(*ppTexture, "D3DX FromFileInMemoryEx");
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileInMemory(IDirect3DDevice9 *device, const void *src, UINT size, IDirect3DTexture9 **ppTexture)
    {
        HRESULT hr = get().m_orig_d3dx_from_memory(device, src, size, ppTexture);
        if (SUCCEEDED(hr) && ppTexture != nullptr && !s_inside_injection)
            register_loaded_texture(*ppTexture, "D3DX FromFileInMemory");
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileExA(IDirect3DDevice9 *device, const char *file, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, void *palette, IDirect3DTexture9 **ppTexture)
    {
        HRESULT hr = get().m_orig_d3dx_from_file_ex_a(device, file, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, palette, ppTexture);
        if (SUCCEEDED(hr) && ppTexture != nullptr && !s_inside_injection)
            register_loaded_texture(*ppTexture, "D3DX FromFileExA");
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileExW(IDirect3DDevice9 *device, const wchar_t *file, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, void *palette, IDirect3DTexture9 **ppTexture)
    {
        HRESULT hr = get().m_orig_d3dx_from_file_ex_w(device, file, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, palette, ppTexture);
        if (SUCCEEDED(hr) && ppTexture != nullptr && !s_inside_injection)
            register_loaded_texture(*ppTexture, "D3DX FromFileExW");
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileA(IDirect3DDevice9 *device, const char *file, IDirect3DTexture9 **ppTexture)
    {
        HRESULT hr = get().m_orig_d3dx_from_file_a(device, file, ppTexture);
        if (SUCCEEDED(hr) && ppTexture != nullptr && !s_inside_injection)
            register_loaded_texture(*ppTexture, "D3DX FromFileA");
        return hr;
    }

    HRESULT WINAPI D3D9Hook::Hooked_D3DXCreateTextureFromFileW(IDirect3DDevice9 *device, const wchar_t *file, IDirect3DTexture9 **ppTexture)
    {
        HRESULT hr = get().m_orig_d3dx_from_file_w(device, file, ppTexture);
        if (SUCCEEDED(hr) && ppTexture != nullptr && !s_inside_injection)
            register_loaded_texture(*ppTexture, "D3DX FromFileW");
        return hr;
    }

    void D3D9Hook::hook_d3dx()
    {
        if (m_d3dx_hooked)
            return;

        // Only a module the game has ALREADY loaded is touched. GetModuleHandleA does not load
        // anything, so a game that ships no D3DX is left exactly as it was.
        HMODULE d3dx = nullptr;
        char module_name[32] = {};
        for (int v = 43; v >= 24 && d3dx == nullptr; --v)
        {
            snprintf(module_name, sizeof(module_name), "d3dx9_%d.dll", v);
            d3dx = GetModuleHandleA(module_name);
        }
        if (d3dx == nullptr)
        {
            d3dx = GetModuleHandleA("d3dx9.dll");
            if (d3dx != nullptr)
                snprintf(module_name, sizeof(module_name), "d3dx9.dll");
        }

        if (d3dx == nullptr)
        {
            // Said once, not on every retry: most games reach here and it is not a problem.
            static std::atomic<bool> s_said{false};
            bool expected = false;
            if (s_said.compare_exchange_strong(expected, true))
                Logger::get().info("[D3D9Hook] No d3dx9 module is loaded in this process; D3DX texture loading is not watched.");
            return;
        }

        m_d3dx_hooked = true;

        struct Entry { const char *name; void *hook; void **orig; };
        const Entry entries[] = {
            { "D3DXCreateTextureFromFileInMemoryEx", &Hooked_D3DXCreateTextureFromFileInMemoryEx, reinterpret_cast<void **>(&m_orig_d3dx_from_memory_ex) },
            { "D3DXCreateTextureFromFileInMemory",   &Hooked_D3DXCreateTextureFromFileInMemory,   reinterpret_cast<void **>(&m_orig_d3dx_from_memory) },
            { "D3DXCreateTextureFromFileExA",        &Hooked_D3DXCreateTextureFromFileExA,        reinterpret_cast<void **>(&m_orig_d3dx_from_file_ex_a) },
            { "D3DXCreateTextureFromFileExW",        &Hooked_D3DXCreateTextureFromFileExW,        reinterpret_cast<void **>(&m_orig_d3dx_from_file_ex_w) },
            { "D3DXCreateTextureFromFileA",          &Hooked_D3DXCreateTextureFromFileA,          reinterpret_cast<void **>(&m_orig_d3dx_from_file_a) },
            { "D3DXCreateTextureFromFileW",          &Hooked_D3DXCreateTextureFromFileW,          reinterpret_cast<void **>(&m_orig_d3dx_from_file_w) },
        };

        int hooked = 0;
        for (const Entry &e : entries)
        {
            void *addr = reinterpret_cast<void *>(GetProcAddress(d3dx, e.name));
            if (addr != nullptr && HookManager::get().create_hook(addr, e.hook, e.orig))
                ++hooked;
        }

        Logger::get().info(std::string("[D3D9Hook] Watching ") + std::to_string(hooked) +
                           " D3DX texture loader(s) in " + module_name + ".");
    }

    // Carries the content tag from a staged surface to the surface actually rendered. Both of
    // these copy pixels without any lock, which is how art can reach the GPU completely unseen.
    static void carry_tag_between_surfaces(IDirect3DSurface9 *src, IDirect3DSurface9 *dst)
    {
        if (src == nullptr || dst == nullptr)
            return;

        IDirect3DTexture9 *src_tex = nullptr;
        if (FAILED(src->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&src_tex))) || src_tex == nullptr)
            return;

        IDirect3DTexture9 *dst_tex = nullptr;
        if (SUCCEEDED(dst->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&dst_tex))) && dst_tex != nullptr)
        {
            TextureManager::get().copy_tag9(src_tex, dst_tex);
            dst_tex->Release();
        }
        src_tex->Release();
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_UpdateSurface(IDirect3DDevice9 *device, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint)
    {
        HRESULT hr = get().m_orig_update_surface(device, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);

        // The same idea as UpdateTexture, one surface at a time: art staged in SYSTEMMEM is copied
        // into the DEFAULT texture the game actually binds. Only a whole-surface copy keeps the
        // identity; a partial one produces different pixels and so a different texture.
        if (SUCCEEDED(hr) && !s_inside_injection)
        {
            log_texture_event("D3D9Hook", (pSourceRect == nullptr) ? "UpdateSurface" : "UpdateSurface (sub-rect)",
                              pDestinationSurface, 0, 0);
            if (pSourceRect == nullptr)
                carry_tag_between_surfaces(pSourceSurface, pDestinationSurface);
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_StretchRect(IDirect3DDevice9 *device, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
    {
        HRESULT hr = get().m_orig_stretch_rect(device, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);

        // Only a straight, whole-surface blit carries the identity across. A scaled or partial one
        // resamples the pixels, so the destination is genuinely a different texture.
        if (SUCCEEDED(hr) && !s_inside_injection)
        {
            const bool whole = (pSourceRect == nullptr && pDestRect == nullptr);
            log_texture_event("D3D9Hook", whole ? "StretchRect" : "StretchRect (partial)", pDestSurface, 0, 0);
            if (whole)
                carry_tag_between_surfaces(pSourceSurface, pDestSurface);
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SurfaceLockRect(IDirect3DSurface9 *surface, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
    {
        const auto *hooks = s_surface_hooks.find(surface_lock_slot(surface));
        if (hooks == nullptr)
            return D3DERR_INVALIDCALL;

        HRESULT hr = hooks->lock_tramp(surface, pLockedRect, pRect, Flags);

        // Skip tracking when we're inside injection
        if (s_inside_injection)
            return hr;

        if (SUCCEEDED(hr) && pLockedRect != nullptr && pLockedRect->pBits != nullptr)
        {
            IDirect3DTexture9 *texture = nullptr;
            if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&texture))) || texture == nullptr)
            {
                // A surface with no parent texture: an offscreen-plain or render-target surface,
                // which a game can fill and then copy into a texture with UpdateSurface or
                // StretchRect. We cannot hash it here (there is no texture to tag), but staying
                // silent about it made this path invisible while diagnosing a game whose art never
                // reached a texture lock at all.
                D3DSURFACE_DESC sdesc = {};
                if (SUCCEEDED(surface->GetDesc(&sdesc)))
                    log_texture_event("D3D9Hook", "SurfaceLockRect on a surface with no parent texture",
                                      surface, sdesc.Width, sdesc.Height);
                return hr;
            }

            {
                D3DSURFACE_DESC desc = {};
                if (SUCCEEDED(surface->GetDesc(&desc)))
                {
                    // Logged before the level-0 test below, which used to return first and so hid
                    // every mip upload from the log entirely.
                    log_texture_event("D3D9Hook", "SurfaceLockRect", texture, desc.Width, desc.Height);

                    // Only the top level identifies the texture. A game uploads its mip chain one
                    // surface at a time (GetSurfaceLevel(n) -> LockRect), and every one of those
                    // arrives here with the SAME parent texture, so without this check each mip is
                    // registered as its own texture AND the last (smallest) one wins the private-data
                    // hash tag. Bind-time lookup then searches for the 16x16 mip's hash and misses
                    // the replacement built for mip 0 -- injection silently did nothing on every
                    // mipmapped texture. Mip levels always differ in size, so comparing against
                    // level 0 identifies the top level exactly.
                    D3DSURFACE_DESC level0 = {};
                    if (FAILED(texture->GetLevelDesc(0, &level0)) ||
                        desc.Width != level0.Width || desc.Height != level0.Height)
                    {
                        texture->Release();
                        return hr;
                    }

                    // Check if pRect covers the full surface
                    if (pRect != nullptr)
                    {
                        UINT rect_w = pRect->right - pRect->left;
                        UINT rect_h = pRect->bottom - pRect->top;
                        if (rect_w != desc.Width || rect_h != desc.Height)
                        {
                            texture->Release();
                            return hr;
                        }
                    }

                    // Ignore dynamic textures (UI, fonts, frequently updated textures)
                    if ((desc.Usage & D3DUSAGE_DYNAMIC) == 0)
                    {
                        LockedTextureData data;
                        data.texture = texture;
                        data.width = desc.Width;
                        data.height = desc.Height;
                        data.format = desc.Format;
                        data.rect = *pLockedRect;

                        s_locked_textures[texture] = data;
                    }
                }
                texture->Release();
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SurfaceUnlockRect(IDirect3DSurface9 *surface)
    {
        // Skip processing when we're inside injection
        if (s_inside_injection)
            return surface_unlock_original(surface);

        IDirect3DTexture9 *texture = nullptr;
        if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&texture))) && texture != nullptr)
        {
            auto it = s_locked_textures.find(texture);
            if (it != s_locked_textures.end())
            {
                LockedTextureData &data = it->second;
                log_texture_event("D3D9Hook", "SurfaceUnlockRect registering", texture, data.width, data.height);

                // Pitch is signed here and unsigned from there on, so a negative one would become
                // an enormous row length instead of a refused lock.
                if (data.rect.Pitch > 0)
                {
                    TextureManager::get().register_unmap_texture9(
                        get().m_device,
                        texture,
                        data.rect.pBits,
                        data.width,
                        data.height,
                        data.format,
                        static_cast<UINT>(data.rect.Pitch)
                    );
                }

                s_locked_textures.erase(it);
            }
            texture->Release();
        }

        return surface_unlock_original(surface);
    }
}
