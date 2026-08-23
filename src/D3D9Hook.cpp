#include "D3D9Hook.h"
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
        void *update_tex_addr = vtable[31]; // IDirect3DDevice9::UpdateTexture
        void *set_tex_addr = vtable[65];

        HookManager::get().create_hook(present_addr, &Hooked_Present, reinterpret_cast<void **>(&m_orig_present));
        HookManager::get().create_hook(reset_addr, &Hooked_Reset, reinterpret_cast<void **>(&m_orig_reset));
        HookManager::get().create_hook(create_tex_addr, &Hooked_CreateTexture, reinterpret_cast<void **>(&m_orig_create_texture));
        HookManager::get().create_hook(update_tex_addr, &Hooked_UpdateTexture, reinterpret_cast<void **>(&m_orig_update_texture));
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
        // Skip hooking new vtables when we're inside injection (creating replacement textures)
        if (s_inside_injection)
            return get().m_orig_create_texture(device, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);

        Logger::get().debug("[D3D9Hook] Hooked_CreateTexture called: Width=" + std::to_string(Width) + ", Height=" + std::to_string(Height) + ", Format=" + std::to_string(static_cast<uint32_t>(Format)));
        HRESULT hr = get().m_orig_create_texture(device, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);

        if (SUCCEEDED(hr) && ppTexture != nullptr && *ppTexture != nullptr)
        {
            IDirect3DTexture9 *tex = *ppTexture;
            void **vtable = *reinterpret_cast<void ***>(tex);

            void *lock_addr = vtable[19];
            void *unlock_addr = vtable[20];

            if (get().m_orig_lock_rect == nullptr)
            {
                HookManager::get().create_hook(lock_addr, &Hooked_LockRect, reinterpret_cast<void **>(&get().m_orig_lock_rect));
                HookManager::get().create_hook(unlock_addr, &Hooked_UnlockRect, reinterpret_cast<void **>(&get().m_orig_unlock_rect));
                Logger::get().info("[D3D9Hook] Hooked LockRect and UnlockRect on the first created texture.");
            }

            IDirect3DSurface9 *pSurface = nullptr;
            if (SUCCEEDED(tex->GetSurfaceLevel(0, &pSurface)) && pSurface != nullptr)
            {
                void **surface_vtable = *reinterpret_cast<void ***>(pSurface);
                void *surf_lock_addr = surface_vtable[13];
                void *surf_unlock_addr = surface_vtable[14];

                if (get().m_orig_surface_lock_rect == nullptr)
                {
                    HookManager::get().create_hook(surf_lock_addr, &Hooked_SurfaceLockRect, reinterpret_cast<void **>(&get().m_orig_surface_lock_rect));
                    HookManager::get().create_hook(surf_unlock_addr, &Hooked_SurfaceUnlockRect, reinterpret_cast<void **>(&get().m_orig_surface_unlock_rect));
                    Logger::get().info("[D3D9Hook] Hooked Surface LockRect and UnlockRect on the first surface.");
                }

                pSurface->Release();
            }
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_LockRect(IDirect3DTexture9 *texture, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
    {
        HRESULT hr = get().m_orig_lock_rect(texture, Level, pLockedRect, pRect, Flags);

        // Skip tracking when we're inside injection (locking replacement textures)
        if (s_inside_injection)
            return hr;

        if (SUCCEEDED(hr) && Level == 0 && pLockedRect != nullptr && pLockedRect->pBits != nullptr)
        {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(texture->GetLevelDesc(0, &desc)))
            {
                static int s_logged_locks = 0;
                if (s_logged_locks < 20)
                {
                    s_logged_locks++;
                    std::string has_rect = (pRect != nullptr) ? "Yes" : "No";
                    Logger::get().debug("[D3D9Hook] Hooked_LockRect: Texture=0x" + std::to_string(reinterpret_cast<uintptr_t>(texture)) + " Width=" + std::to_string(desc.Width) + " Height=" + std::to_string(desc.Height) + " pRect=" + has_rect);
                }

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
            return get().m_orig_unlock_rect(texture, Level);

        if (Level == 0)
        {
            auto it = s_locked_textures.find(texture);
            if (it != s_locked_textures.end())
            {
                LockedTextureData &data = it->second;
                static int s_logged_unlocks = 0;
                if (s_logged_unlocks < 20)
                {
                    s_logged_unlocks++;
                    Logger::get().debug("[D3D9Hook] Hooked_UnlockRect: Registering texture=0x" + std::to_string(reinterpret_cast<uintptr_t>(texture)));
                }

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

        return get().m_orig_unlock_rect(texture, Level);
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage, IDirect3DBaseTexture9 *pTexture)
    {
        static int s_logged_set_texture_calls = 0;
        if (s_logged_set_texture_calls < 20 && pTexture != nullptr)
        {
            s_logged_set_texture_calls++;
            Logger::get().debug("[D3D9Hook] Hooked_SetTexture: Stage=" + std::to_string(Stage) + " pTexture=0x" + std::to_string(reinterpret_cast<uintptr_t>(pTexture)));
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
            TextureManager::get().copy_tag9(pSourceTexture, pDestinationTexture);

        return hr;
    }

    HRESULT STDMETHODCALLTYPE D3D9Hook::Hooked_SurfaceLockRect(IDirect3DSurface9 *surface, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
    {
        HRESULT hr = get().m_orig_surface_lock_rect(surface, pLockedRect, pRect, Flags);

        // Skip tracking when we're inside injection
        if (s_inside_injection)
            return hr;

        if (SUCCEEDED(hr) && pLockedRect != nullptr && pLockedRect->pBits != nullptr)
        {
            IDirect3DTexture9 *texture = nullptr;
            if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&texture))) && texture != nullptr)
            {
                D3DSURFACE_DESC desc = {};
                if (SUCCEEDED(surface->GetDesc(&desc)))
                {
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

                    static int s_logged_surf_locks = 0;
                    if (s_logged_surf_locks < 20)
                    {
                        s_logged_surf_locks++;
                        std::string has_rect = (pRect != nullptr) ? "Yes" : "No";
                        Logger::get().debug("[D3D9Hook] Hooked_SurfaceLockRect: Surface=0x" + std::to_string(reinterpret_cast<uintptr_t>(surface)) + " ParentTexture=0x" + std::to_string(reinterpret_cast<uintptr_t>(texture)) + " Width=" + std::to_string(desc.Width) + " Height=" + std::to_string(desc.Height) + " pRect=" + has_rect);
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
            return get().m_orig_surface_unlock_rect(surface);

        IDirect3DTexture9 *texture = nullptr;
        if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&texture))) && texture != nullptr)
        {
            auto it = s_locked_textures.find(texture);
            if (it != s_locked_textures.end())
            {
                LockedTextureData &data = it->second;
                static int s_logged_surf_unlocks = 0;
                if (s_logged_surf_unlocks < 20)
                {
                    s_logged_surf_unlocks++;
                    Logger::get().debug("[D3D9Hook] Hooked_SurfaceUnlockRect: Registering parent texture=0x" + std::to_string(reinterpret_cast<uintptr_t>(texture)));
                }

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

        return get().m_orig_surface_unlock_rect(surface);
    }
}
