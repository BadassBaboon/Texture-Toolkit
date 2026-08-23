#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace TextureToolkit
{
    class D3D11Hook
    {
    public:
        static D3D11Hook &get();

        bool init();
        void shutdown();

        void hook_swapchain(IDXGISwapChain *swapchain);
        void hook_context(ID3D11DeviceContext *context);
        void hook_device(ID3D11Device *device);
        void hook_dxgi_factory(IDXGIFactory *factory);

        // Creates a throwaway swapchain to hook the shared IDXGISwapChain::Present vtable slot,
        // so the game's Present is intercepted regardless of how/where it creates its swapchain
        // (flip-model, a factory we never saw created, or a proxy dxgi.dll). Runs once, off the
        // loader lock. See the .cpp for why this is more reliable than hooking factory creation.
        void bootstrap_dxgi_present();

        ID3D11Device *get_device() const { return m_device; }
        bool overlay_ready() const { return m_imgui_initialized; }

        // See D3D9Hook::s_present_count.
        static std::atomic<uint64_t> s_present_count;
        ID3D11DeviceContext *get_context() const { return m_context; }

        // Re-entrancy guard for injection
        static thread_local bool s_inside_injection;

    private:
        D3D11Hook() = default;
        ~D3D11Hook();

        typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(
            IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
            const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

        typedef HRESULT(WINAPI *D3D11CreateDevice_t)(
            IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
            ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

        typedef HRESULT(WINAPI *CreateDXGIFactory_t)(REFIID, void **);
        // DXGI 1.3. Modern games call this one, and it takes a Flags argument the others do not.
        typedef HRESULT(WINAPI *CreateDXGIFactory2_t)(UINT, REFIID, void **);

        typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChain_t)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
        typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainForHwnd_t)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
        typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);
        typedef void(STDMETHODCALLTYPE *PSSetShaderResources_t)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
        typedef HRESULT(STDMETHODCALLTYPE *Map_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
        typedef void(STDMETHODCALLTYPE *Unmap_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT);

        static HRESULT WINAPI Hooked_D3D11CreateDeviceAndSwapChain(
            IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
            const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
            const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
            ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);

        static HRESULT WINAPI Hooked_D3D11CreateDevice(
            IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
            const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
            ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);

        static HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void **ppFactory);
        static HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void **ppFactory);
        static HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);

        static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory *factory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc, IDXGISwapChain **ppSwapChain);
        static HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(IDXGIFactory2 *factory, IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain);
        static HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain *swapchain, UINT SyncInterval, UINT Flags);
        static void STDMETHODCALLTYPE Hooked_PSSetShaderResources(ID3D11DeviceContext *context, UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews);
        static HRESULT STDMETHODCALLTYPE Hooked_Map(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource);
        static void STDMETHODCALLTYPE Hooked_Unmap(ID3D11DeviceContext *context, ID3D11Resource *pResource, UINT Subresource);

        void init_imgui(IDXGISwapChain *swapchain);
        void render_imgui(IDXGISwapChain *swapchain);

        std::mutex m_mutex;
        bool m_initialized = false;
        bool m_imgui_initialized = false;

        IDXGISwapChain *m_swapchain = nullptr;
        ID3D11Device *m_device = nullptr;
        ID3D11DeviceContext *m_context = nullptr;
        HWND m_hwnd = nullptr;

        D3D11CreateDeviceAndSwapChain_t m_orig_create_device_and_swapchain = nullptr;
        D3D11CreateDevice_t m_orig_create_device = nullptr;
        CreateDXGIFactory_t m_orig_create_dxgi_factory = nullptr;
        CreateDXGIFactory_t m_orig_create_dxgi_factory1 = nullptr;
        CreateDXGIFactory2_t m_orig_create_dxgi_factory2 = nullptr;

        typedef HRESULT(STDMETHODCALLTYPE *CreateTexture2D_t)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
        CreateTexture2D_t m_orig_create_texture2d = nullptr;

        CreateSwapChain_t m_orig_create_swapchain = nullptr;
        CreateSwapChainForHwnd_t m_orig_create_swapchain_for_hwnd = nullptr;
        Present_t m_orig_present = nullptr;
        PSSetShaderResources_t m_orig_ps_set_shader_resources = nullptr;
        Map_t m_orig_map = nullptr;
        Unmap_t m_orig_unmap = nullptr;

        static HRESULT STDMETHODCALLTYPE Hooked_CreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D);
    };
}
