// Renders the Texture Toolkit panel headlessly and writes it to a PNG.
//
// The panel only ever runs inside a hooked game, which makes changing how it looks a matter of
// rebuilding, launching a game, and squinting. This drives the real draw_ui() against the real
// TextureManager on an offscreen D3D11 device instead, with textures registered through the same
// path a game's uploads take, so every status shows for real rather than being faked.
//
//   TextureToolkitUIPreview out.png [frames] [action...]
//
// Actions, applied on the given frame:
//   move:X,Y@F    move the mouse          click:X,Y@F   click at X,Y
//   key:[@F       press [ (or ])          size:WxH      output size (default 1600x900)
//
// Built only with -DTT_BUILD_UI_PREVIEW=ON. Not part of the plugin.

#include "TextureManager.h"
#include "TextureToolkitUI.h"
#include "TextureHash.h"
#include "UITheme.h"
#include "Logo.h"
#include "Config.h"
#include "Logger.h"
#include "DDSLoader.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <d3d11.h>
#include <wincodec.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

HMODULE g_our_module = nullptr;

using namespace TextureToolkit;

namespace
{
    struct Action
    {
        enum Kind { Move, Click, Key, Panel } kind;
        int frame;
        float x = 0, y = 0;
        char key = 0;
    };

    struct FakeTexture
    {
        ID3D11Texture2D *tex = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        bool drawn = true;
    };

    std::filesystem::path exe_dir()
    {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    // Procedural art, varied enough that the thumbnails look like textures and not noise.
    std::vector<uint8_t> make_pixels(uint32_t w, uint32_t h, int kind, bool alpha)
    {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const float u = static_cast<float>(x) / w, v = static_cast<float>(y) / h;
                float r = 0, g = 0, b = 0, a = 1;
                switch (kind % 6)
                {
                case 0: r = 0.25f + 0.6f * u; g = 0.35f + 0.4f * v; b = 0.85f - 0.4f * u; break;
                case 1: { const bool c = ((x / (w / 8 + 1)) + (y / (h / 8 + 1))) & 1; r = c ? 0.86f : 0.22f; g = c ? 0.70f : 0.18f; b = c ? 0.42f : 0.15f; break; }
                case 2: { const float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)); r = 0.9f - d; g = 0.4f + 0.3f * std::sin(d * 30.0f); b = 0.3f + d; break; }
                case 3: r = 0.55f + 0.35f * std::sin(u * 12.0f); g = 0.55f + 0.35f * std::sin(v * 9.0f); b = 0.65f; break;
                case 4: { const float s = std::sin(u * 40.0f) * std::sin(v * 40.0f); r = g = b = 0.45f + 0.35f * s; g += 0.05f; break; }
                default: r = 0.15f + 0.7f * v; g = 0.75f - 0.5f * v; b = 0.35f + 0.4f * u; break;
                }
                if (alpha)
                {
                    const float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
                    a = d < 0.42f ? 1.0f : (d < 0.48f ? (0.48f - d) / 0.06f : 0.0f);
                }
                uint8_t *p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = static_cast<uint8_t>((std::min)(1.0f, (std::max)(0.0f, r)) * 255);
                p[1] = static_cast<uint8_t>((std::min)(1.0f, (std::max)(0.0f, g)) * 255);
                p[2] = static_cast<uint8_t>((std::min)(1.0f, (std::max)(0.0f, b)) * 255);
                p[3] = static_cast<uint8_t>(a * 255);
            }
        }
        return px;
    }

    bool write_png(const std::wstring &path, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t pitch)
    {
        IWICImagingFactory *factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return false;
        IWICStream *stream = nullptr;
        IWICBitmapEncoder *enc = nullptr;
        IWICBitmapFrameEncode *frame = nullptr;
        bool ok = SUCCEEDED(factory->CreateStream(&stream)) &&
                  SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                  SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                  SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
                  SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                  SUCCEEDED(frame->Initialize(nullptr)) &&
                  SUCCEEDED(frame->SetSize(w, h));
        if (ok)
        {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
                 SUCCEEDED(frame->WritePixels(h, pitch, pitch * h, const_cast<BYTE *>(bgra))) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
        if (frame) frame->Release();
        if (enc) enc->Release();
        if (stream) stream->Release();
        factory->Release();
        return ok;
    }

    std::vector<Action> parse_actions(int argc, char **argv, int first, uint32_t &w, uint32_t &h)
    {
        std::vector<Action> out;
        for (int i = first; i < argc; ++i)
        {
            const std::string a = argv[i];
            float x = 0, y = 0;
            int f = 0;
            char k = 0;
            unsigned sw = 0, sh = 0;
            if (std::sscanf(a.c_str(), "move:%f,%f@%d", &x, &y, &f) == 3)
                out.push_back({ Action::Move, f, x, y });
            else if (std::sscanf(a.c_str(), "click:%f,%f@%d", &x, &y, &f) == 3)
                out.push_back({ Action::Click, f, x, y });
            else if (std::sscanf(a.c_str(), "key:%c@%d", &k, &f) == 2)
                out.push_back({ Action::Key, f, 0, 0, k });
            else if (std::sscanf(a.c_str(), "panel:%c@%d", &k, &f) == 2)
                out.push_back({ Action::Panel, f, 0, 0, k });
            else if (std::sscanf(a.c_str(), "size:%ux%u", &sw, &sh) == 2)
            {
                w = sw;
                h = sh;
            }
            else
                std::fprintf(stderr, "ignored argument: %s\n", a.c_str());
        }
        return out;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: TextureToolkitUIPreview out.png [frames] [move:X,Y@F] [click:X,Y@F] [key:]@F] [size:WxH]\n");
        return 2;
    }
    const std::filesystem::path out_path = std::filesystem::absolute(argv[1]);
    const int frames = (argc >= 3 && std::isdigit(static_cast<unsigned char>(argv[2][0]))) ? std::atoi(argv[2]) : 30;
    uint32_t W = 1600, H = 900;
    const std::vector<Action> actions = parse_actions(argc, argv, (argc >= 3 && std::isdigit(static_cast<unsigned char>(argv[2][0]))) ? 3 : 2, W, H);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // A clean sandbox next to the exe, so a run never inherits the last one's files.
    const std::filesystem::path dir = exe_dir();
    std::error_code ec;
    std::filesystem::remove_all(dir / "TT", ec);
    std::filesystem::remove(dir / "TextureToolkit.ini", ec);

    Logger::get().init(dir);
    ConfigManager::get().init(dir);
    Configuration &cfg = ConfigManager::get().get_config();
    cfg.show_current_frame_only = false;
    TextureManager::get().init();

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    {
        std::fprintf(stderr, "no D3D11 device\n");
        return 1;
    }

    // ---- textures, prepared so each status turns up through the real pipeline -------------
    struct Spec { uint32_t w, h; int kind; bool alpha; enum { Plain, Inject, BrokenInject, Dump, Hidden } role; };
    const Spec specs[] = {
        { 1024, 1024, 0, false, Spec::Inject },  { 512, 512, 2, true, Spec::Plain },   { 2048, 1024, 3, false, Spec::Plain },
        { 256, 256, 1, false, Spec::Dump },      { 512, 256, 4, false, Spec::Plain },  { 128, 128, 5, true, Spec::BrokenInject },
        { 1024, 512, 2, false, Spec::Plain },    { 64, 64, 0, false, Spec::Plain },    { 512, 512, 3, true, Spec::Inject },
        { 256, 128, 1, false, Spec::Plain },     { 2048, 2048, 4, false, Spec::Plain }, { 128, 64, 5, false, Spec::Hidden },
        { 512, 512, 0, false, Spec::Plain },     { 32, 32, 2, false, Spec::Plain },    { 1024, 1024, 5, false, Spec::Plain },
        { 256, 256, 3, true, Spec::Plain },      { 512, 128, 4, false, Spec::Plain },  { 128, 128, 1, false, Spec::Plain },
    };

    TextureManager &tm = TextureManager::get();
    const std::filesystem::path inject = tm.get_inject_dir();

    std::vector<std::vector<uint8_t>> all_pixels;
    for (const Spec &s : specs)
    {
        all_pixels.push_back(make_pixels(s.w, s.h, s.kind, s.alpha));
        const std::vector<uint8_t> &px = all_pixels.back();
        const uint64_t hash = compute_hash64_rows(px.data(), s.w * 4, s.w * 4, s.h);
        const std::string name = format_hash_hex(hash) + ".dds";

        if (s.role == Spec::Inject)
        {
            // The replacement is the same art at twice the size, recoloured, so it reads as
            // different from the original in the preview.
            std::vector<uint8_t> big = make_pixels(s.w * 2, s.h * 2, s.kind + 3, s.alpha);
            reshade::api::resource_desc desc;
            desc.texture.width = s.w * 2;
            desc.texture.height = s.h * 2;
            desc.texture.format = reshade::api::format::r8g8b8a8_unorm;
            reshade::api::subresource_data sub;
            sub.data = big.data();
            sub.row_pitch = s.w * 2 * 4;
            save_dds_multi_mip((inject / name).string(), desc, { sub }, 1, 1);
        }
        else if (s.role == Spec::BrokenInject)
        {
            std::FILE *f = nullptr;
            if (_wfopen_s(&f, (inject / name).c_str(), L"wb") == 0 && f)
            {
                std::fwrite("DDS  truncated on purpose", 1, 25, f);
                std::fclose(f);
            }
        }
    }
    tm.rescan_injected();

    std::vector<FakeTexture> fakes;
    for (size_t i = 0; i < std::size(specs); ++i)
    {
        const Spec &s = specs[i];
        const std::vector<uint8_t> &px = all_pixels[i];

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = s.w;
        td.Height = s.h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { px.data(), s.w * 4, 0 };

        FakeTexture ft;
        dev->CreateTexture2D(&td, &init, &ft.tex);
        dev->CreateShaderResourceView(ft.tex, nullptr, &ft.srv);
        ft.drawn = (s.role != Spec::Hidden);

        tm.auto_dump = (s.role == Spec::Dump);
        tm.register_unmap_texture11(dev, ft.tex, px.data(), s.w, s.h, DXGI_FORMAT_R8G8B8A8_UNORM, s.w * 4);
        tm.auto_dump = false;
        fakes.push_back(ft);
    }

    // ---- ImGui, exactly as the hooks set it up -------------------------------------------
    ImGui::CreateContext();
    UI::init();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(W), static_cast<float>(H));
    ImGui_ImplDX11_Init(dev, ctx);
    Logo::create_d3d11(dev);

    D3D11_TEXTURE2D_DESC rtd = {};
    rtd.Width = W;
    rtd.Height = H;
    rtd.MipLevels = 1;
    rtd.ArraySize = 1;
    rtd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtd.SampleDesc.Count = 1;
    rtd.Usage = D3D11_USAGE_DEFAULT;
    rtd.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    dev->CreateTexture2D(&rtd, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);

    TextureToolkitUI::set_visible(true);

    for (int f = 0; f < frames; ++f)
    {
        io.DeltaTime = 1.0f / 60.0f;
        Sleep(16); // the startup banner times itself by the wall clock, not by frames
        for (const Action &a : actions)
        {
            if (a.kind == Action::Move && a.frame == f)
                io.AddMousePosEvent(a.x, a.y);
            if (a.kind == Action::Click && a.frame == f)
            {
                io.AddMousePosEvent(a.x, a.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (a.kind == Action::Click && a.frame + 1 == f)
                io.AddMouseButtonEvent(0, false);
            if (a.kind == Action::Key && (a.frame == f || a.frame + 1 == f))
                io.AddKeyEvent(a.key == '[' ? ImGuiKey_LeftBracket : ImGuiKey_RightBracket, a.frame == f);
            if (a.kind == Action::Panel && a.frame == f)
                TextureToolkitUI::set_visible(a.key == '1');
        }

        // What a game's draw calls do: bind each visible texture, which is what marks it as
        // in the scene and lets the panel pin a live preview of it.
        for (const FakeTexture &ft : fakes)
            if (ft.drawn)
                tm.get_replacement_srv11(ft.srv);
        tm.on_frame();

        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();

        // Something game-like behind the panel, so its translucency shows.
        ImDrawList *bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, IM_COL32(48, 64, 92, 255), IM_COL32(120, 92, 70, 255),
                                    IM_COL32(40, 36, 44, 255), IM_COL32(26, 40, 58, 255));
        for (int i = 0; i < 9; ++i)
            bg->AddCircleFilled(ImVec2(W * (0.1f + 0.1f * i), H * (0.3f + 0.05f * (i % 4))), 60.0f + 14.0f * (i % 3),
                                IM_COL32(200, 180, 140, 40));

        TextureToolkitUI::draw_ui();
        ImGui::Render();

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // ---- read back and save ----------------------------------------------------------------
    D3D11_TEXTURE2D_DESC sd = rtd;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &staging);
    ctx->CopyResource(staging, rt);
    D3D11_MAPPED_SUBRESOURCE m = {};
    int rc = 1;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        std::vector<uint8_t> px(static_cast<size_t>(m.RowPitch) * H);
        std::memcpy(px.data(), m.pData, px.size());
        ctx->Unmap(staging, 0);
        for (size_t i = 3; i < px.size(); i += 4)
            px[i] = 255;
        if (write_png(out_path.wstring(), px.data(), W, H, m.RowPitch))
        {
            std::printf("wrote %s (%ux%u, %d frames)\n", out_path.string().c_str(), W, H, frames);
            rc = 0;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    TextureManager::get().shutdown();
    return rc;
}
