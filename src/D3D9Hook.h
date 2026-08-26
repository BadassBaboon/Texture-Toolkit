#pragma once

#include <d3d9.h>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace TextureToolkit
{
    class D3D9Hook
    {
    public:
        static D3D9Hook &get();

        bool init();
        void shutdown();

        void hook_d3d9_interface(IDirect3D9 *d3d9);
        void hook_device_interface(IDirect3DDevice9 *device);

        // Hooks the D3DX texture loaders in whatever d3dx9_*.dll the game has ALREADY loaded.
        // Nothing is ever LoadLibrary'd: a game that does not ship D3DX keeps the lock-based path
        // untouched. Games from the D3DX era hand a file to D3DX and never lock the texture
        // themselves, so without this their art is created but never seen (Street Racing Syndicate
        // creates 1487 textures, including 484 DXT1/DXT3, and locks two of them).
        void hook_d3dx();

        IDirect3DDevice9 *get_device() const { return m_device; }
        bool overlay_ready() const { return m_imgui_initialized; }

        // Frames this hook has presented. Read by the startup watchdog to tell "we are not on the
        // game's render path" apart from "the game never rendered".
        static std::atomic<uint64_t> s_present_count;

        // Re-entrancy guard: set true while creating replacement textures
        // to prevent our hooks from re-entering the injection path
        static thread_local bool s_inside_injection;

    private:
        D3D9Hook() = default;
        ~D3D9Hook();

        typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
        typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
        typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
        typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
        typedef HRESULT(STDMETHODCALLTYPE *PresentEx_t)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
        typedef HRESULT(STDMETHODCALLTYPE *SwapChainPresent_t)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
        typedef HRESULT(STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
        typedef HRESULT(STDMETHODCALLTYPE *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
        typedef HRESULT(STDMETHODCALLTYPE *LockRect_t)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
        typedef HRESULT(STDMETHODCALLTYPE *UnlockRect_t)(IDirect3DTexture9 *, UINT);
        typedef HRESULT(STDMETHODCALLTYPE *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
        typedef HRESULT(STDMETHODCALLTYPE *UpdateTexture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);

        typedef HRESULT(STDMETHODCALLTYPE *UpdateSurface_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *);
        typedef HRESULT(STDMETHODCALLTYPE *StretchRect_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);

        // D3DX loaders. Declared with void* for the D3DX-only structs (D3DXIMAGE_INFO, PALETTEENTRY)
        // so none of this needs the D3DX SDK headers, which are not a dependency of this project.
        typedef HRESULT(WINAPI *D3DXCreateTextureFromFileInMemoryEx_t)(IDirect3DDevice9 *, const void *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void *, void *, IDirect3DTexture9 **);
        typedef HRESULT(WINAPI *D3DXCreateTextureFromFileInMemory_t)(IDirect3DDevice9 *, const void *, UINT, IDirect3DTexture9 **);
        typedef HRESULT(WINAPI *D3DXCreateTextureFromFileExA_t)(IDirect3DDevice9 *, const char *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void *, void *, IDirect3DTexture9 **);
        typedef HRESULT(WINAPI *D3DXCreateTextureFromFileExW_t)(IDirect3DDevice9 *, const wchar_t *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void *, void *, IDirect3DTexture9 **);
        typedef HRESULT(WINAPI *D3DXCreateTextureFromFileA_t)(IDirect3DDevice9 *, const char *, IDirect3DTexture9 **);
        typedef HRESULT(WINAPI *D3DXCreateTextureFromFileW_t)(IDirect3DDevice9 *, const wchar_t *, IDirect3DTexture9 **);

        // Surface Hooks
        typedef HRESULT(STDMETHODCALLTYPE *SurfaceLockRect_t)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
        typedef HRESULT(STDMETHODCALLTYPE *SurfaceUnlockRect_t)(IDirect3DSurface9 *);

        static IDirect3D9 *WINAPI Hooked_Direct3DCreate9(UINT SDKVersion);
        static HRESULT WINAPI Hooked_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D);
        static HRESULT WINAPI Hooked_CreateDevice(IDirect3D9 *d3d9, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnDeviceInterface);
        static HRESULT STDMETHODCALLTYPE Hooked_Present(IDirect3DDevice9 *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion);
        static HRESULT STDMETHODCALLTYPE Hooked_PresentEx(IDirect3DDevice9Ex *device, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags);
        static HRESULT STDMETHODCALLTYPE Hooked_SwapChainPresent(IDirect3DSwapChain9 *swapchain, const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags);
        static HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture(IDirect3DDevice9 *device, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle);
        static HRESULT STDMETHODCALLTYPE Hooked_LockRect(IDirect3DTexture9 *texture, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
        static HRESULT STDMETHODCALLTYPE Hooked_UnlockRect(IDirect3DTexture9 *texture, UINT Level);
        static HRESULT STDMETHODCALLTYPE Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage, IDirect3DBaseTexture9 *pTexture);
        static HRESULT STDMETHODCALLTYPE Hooked_UpdateTexture(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture);

        static HRESULT STDMETHODCALLTYPE Hooked_UpdateSurface(IDirect3DDevice9 *device, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint);
        static HRESULT STDMETHODCALLTYPE Hooked_StretchRect(IDirect3DDevice9 *device, IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter);

        static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileInMemoryEx(IDirect3DDevice9 *device, const void *src, UINT size, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, void *palette, IDirect3DTexture9 **ppTexture);
        static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileInMemory(IDirect3DDevice9 *device, const void *src, UINT size, IDirect3DTexture9 **ppTexture);
        static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileExA(IDirect3DDevice9 *device, const char *file, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, void *palette, IDirect3DTexture9 **ppTexture);
        static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileExW(IDirect3DDevice9 *device, const wchar_t *file, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, void *palette, IDirect3DTexture9 **ppTexture);
        static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileA(IDirect3DDevice9 *device, const char *file, IDirect3DTexture9 **ppTexture);
        static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileW(IDirect3DDevice9 *device, const wchar_t *file, IDirect3DTexture9 **ppTexture);

        // Reads back a texture D3DX has just filled and registers it the way an unlock would.
        static void register_loaded_texture(IDirect3DTexture9 *texture, const char *origin);

        static HRESULT STDMETHODCALLTYPE Hooked_SurfaceLockRect(IDirect3DSurface9 *surface, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
        static HRESULT STDMETHODCALLTYPE Hooked_SurfaceUnlockRect(IDirect3DSurface9 *surface);

        void init_imgui(IDirect3DDevice9 *device);
        void render_imgui(IDirect3DDevice9 *device);

        std::mutex m_mutex;
        bool m_initialized = false;
        bool m_imgui_initialized = false;

        IDirect3DDevice9 *m_device = nullptr;
        HWND m_hwnd = nullptr;

        Direct3DCreate9_t m_orig_direct3d_create9 = nullptr;
        Direct3DCreate9Ex_t m_orig_direct3d_create9_ex = nullptr;
        CreateDevice_t m_orig_create_device = nullptr;
        Present_t m_orig_present = nullptr;
        PresentEx_t m_orig_present_ex = nullptr;
        SwapChainPresent_t m_orig_swapchain_present = nullptr;
        Reset_t m_orig_reset = nullptr;
        CreateTexture_t m_orig_create_texture = nullptr;
        SetTexture_t m_orig_set_texture = nullptr;
        UpdateTexture_t m_orig_update_texture = nullptr;

        UpdateSurface_t m_orig_update_surface = nullptr;
        StretchRect_t m_orig_stretch_rect = nullptr;

        D3DXCreateTextureFromFileInMemoryEx_t m_orig_d3dx_from_memory_ex = nullptr;
        D3DXCreateTextureFromFileInMemory_t m_orig_d3dx_from_memory = nullptr;
        D3DXCreateTextureFromFileExA_t m_orig_d3dx_from_file_ex_a = nullptr;
        D3DXCreateTextureFromFileExW_t m_orig_d3dx_from_file_ex_w = nullptr;
        D3DXCreateTextureFromFileA_t m_orig_d3dx_from_file_a = nullptr;
        D3DXCreateTextureFromFileW_t m_orig_d3dx_from_file_w = nullptr;
        bool m_d3dx_hooked = false;
    };
}
