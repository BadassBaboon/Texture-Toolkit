#include "Logo.h"
#include "LogoData.h"
#include "UITheme.h"
#include "D3D9Hook.h"
#include "D3D11Hook.h"
#include "ScopedFlag.h"
#include "Logger.h"

#include <d3d9.h>
#include <d3d11.h>
#include <cstring>
#include <cmath>

namespace TextureToolkit::Logo
{
    namespace
    {
        IDirect3DTexture9 *g_tex9 = nullptr;
        ID3D11ShaderResourceView *g_srv11 = nullptr;
    }

    void create_d3d9(IDirect3DDevice9 *device)
    {
        if (device == nullptr || g_tex9 != nullptr)
            return;

        // Our own texture: keep the hooks from tracking it as one of the game's.
        ScopedFlag no_reentry(D3D9Hook::s_inside_injection);

        // MANAGED, so it survives a device Reset without being rebuilt.
        IDirect3DTexture9 *tex = nullptr;
        if (FAILED(device->CreateTexture(kLogoSize, kLogoSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || tex == nullptr)
        {
            Logger::get().warn("[Logo] Could not create the D3D9 logo texture; using the drawn badge.");
            return;
        }

        D3DLOCKED_RECT rect = {};
        if (FAILED(tex->LockRect(0, &rect, nullptr, 0)) || rect.pBits == nullptr || rect.Pitch <= 0)
        {
            tex->Release();
            return;
        }
        for (unsigned int y = 0; y < kLogoSize; ++y)
        {
            const unsigned char *src = kLogoRGBA + y * kLogoSize * 4;
            unsigned char *dst = static_cast<unsigned char *>(rect.pBits) + y * static_cast<unsigned int>(rect.Pitch);
            for (unsigned int x = 0; x < kLogoSize; ++x)
            {
                // RGBA in, BGRA (D3DFMT_A8R8G8B8 in memory) out.
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
        tex->UnlockRect(0);
        g_tex9 = tex;
    }

    void create_d3d11(ID3D11Device *device)
    {
        if (device == nullptr || g_srv11 != nullptr)
            return;

        ScopedFlag no_reentry(D3D11Hook::s_inside_injection);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = kLogoSize;
        desc.Height = kLogoSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = kLogoRGBA;
        init.SysMemPitch = kLogoSize * 4;

        ID3D11Texture2D *tex = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, &init, &tex)) || tex == nullptr)
        {
            Logger::get().warn("[Logo] Could not create the D3D11 logo texture; using the drawn badge.");
            return;
        }
        if (FAILED(device->CreateShaderResourceView(tex, nullptr, &g_srv11)))
            g_srv11 = nullptr;
        tex->Release();
    }

    void release()
    {
        if (g_tex9 != nullptr)
        {
            g_tex9->Release();
            g_tex9 = nullptr;
        }
        if (g_srv11 != nullptr)
        {
            g_srv11->Release();
            g_srv11 = nullptr;
        }
    }

    void draw(ImDrawList *dl, ImVec2 pos, float size, float alpha)
    {
        // Whole pixels, so a 2:1 downscale of the stored image samples evenly.
        pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
        const ImVec2 end(pos.x + size, pos.y + size);
        const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f + 0.5f));

        ImTextureID id = ImTextureID_Invalid;
        if (g_srv11 != nullptr)
            id = reinterpret_cast<ImTextureID>(g_srv11);
        else if (g_tex9 != nullptr)
            id = reinterpret_cast<ImTextureID>(g_tex9);

        if (id != ImTextureID_Invalid)
        {
            dl->AddImage(ImTextureRef(id), pos, end, ImVec2(0, 0), ImVec2(1, 1), tint);
            return;
        }

        // Fallback: the badge the panel drew before there was a logo.
        using namespace UI;
        const Palette &p = pal();
        dl->AddRectFilled(pos, end, ImGui::ColorConvertFloat4ToU32(ImVec4(p.accent_fill.x, p.accent_fill.y, p.accent_fill.z, alpha)), size * 0.26f);
        draw_icon(dl, Icon::Layers, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.5f,
                  ImGui::ColorConvertFloat4ToU32(ImVec4(p.accent_text.x, p.accent_text.y, p.accent_text.z, alpha)));
    }
}
