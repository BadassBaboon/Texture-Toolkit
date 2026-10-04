#include "TextureToolkitUI.h"
#include "TextureManager.h"
#include "D3D9Hook.h"
#include "D3D11Hook.h"
#include "OSDBanner.h"
#include "Config.h"
#include "Logger.h"
#include "UITheme.h"
#include "Logo.h"
#include "Environment.h"
#include "PathUtil.h"
#include "HookTimings.h"
#include "Version.h"
#include <windows.h>
#include <cmath>
#include <dxgi.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <utility>
#include <imgui.h>

// Defined in imgui_impl_win32.cpp with external linkage but not declared in its header (see Config.cpp).
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

namespace TextureToolkit
{
    using namespace UI;

    // Set by anything that changes the list and should show up at once (Reload, a dump, a
    // selection-driven injection) instead of waiting for the next snapshot.
    static bool s_force_refresh = true;

    std::atomic<bool> TextureToolkitUI::s_show_ui{false}; // Default hidden; the configured hotkey toggles it
    static std::string s_status_message; // empty until something has happened worth reporting
    static uint64_t s_selected_texture_hash = 0;
    static char s_filter_buf[64] = "";

    enum class Page { Textures, ModFiles, Settings, Diagnostics };

    // Room kept clear at the top right of every page for the window's close button.
    constexpr float kCloseReserve = 44.0f;
    static Page s_page = Page::Textures;

    // When Texture Toolkit loaded, for the session length in Build Info. Real time, not
    // GetTickCount64, which a game's frame-rate unlocker can hook and speed up (NFS: The Run
    // reported a 55-minute session after three).
    static const uint64_t s_session_start = HookTimings::now();

    static const wchar_t *const kDiscordInvite = L"https://discord.gg/qRdVSkUW6n";
    static const char *const kDiscordLabel = "Join Discord";
    static const char *const kBrandTitle = "TEXTURE TOOLKIT";
    static const char *const kBrandCredit = "by BadassBaboon";
    static constexpr float kBrandLogo = 40.0f;
    static constexpr float kBrandGap = 12.0f;

    static void SetStatusMessage(const std::string &msg)
    {
        s_status_message = msg;
        Logger::get().info("[UI] " + msg);
    }

    static void OpenDirectory(const std::filesystem::path &dir_path)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
        ShellExecuteW(nullptr, L"open", dir_path.c_str(), nullptr, nullptr, SW_SHOW);
    }

    // The OS cursor's display count when the panel opened, restored when it closes.
    static bool s_cursor_pinned = false;
    static int s_cursor_saved_count = 0;

    void TextureToolkitUI::release_overlay_mouse()
    {
        if (!s_cursor_pinned)
            return;
        s_cursor_pinned = false;
        ImGui::GetIO().MouseDrawCursor = false;

        int count = ShowCursor(TRUE);
        count = ShowCursor(FALSE); // back where it was, now known
        while (count < s_cursor_saved_count)
            count = ShowCursor(TRUE);
        while (count > s_cursor_saved_count)
            count = ShowCursor(FALSE);
    }

    void TextureToolkitUI::feed_overlay_mouse(HWND hwnd)
    {
        ImGuiIO &io = ImGui::GetIO();

        // Software cursor: the OS hardware cursor is unreliable in fullscreen (the game
        // hides it, e.g. NFS/DX11), so hide it and let ImGui draw the pointer instead.
        io.MouseDrawCursor = true;

        // Pin the OS cursor-display counter at exactly -1 (hidden). A plain
        // ShowCursor(FALSE) every frame would drive the counter unbounded-negative,
        // making it impossible for the game to re-show its cursor afterwards. The count the
        // game had is noted first and put back by release_overlay_mouse when the panel closes;
        // leaving it at -1 took the cursor away from any game that shows the OS one.
        int cursor_count = ShowCursor(FALSE);
        if (!s_cursor_pinned)
        {
            s_cursor_saved_count = cursor_count + 1;
            s_cursor_pinned = true;
        }
        while (cursor_count >= 0)
            cursor_count = ShowCursor(FALSE);
        while (cursor_count < -1)
            cursor_count = ShowCursor(TRUE);

        // Poll the OS for cursor position and button state. Callers invoke this inside
        // the g_inside_imgui_render window, so the hooked GetAsyncKeyState passes through
        // to the real state instead of being masked. This is what makes clicks register
        // under exclusive-DirectInput games that post no window button messages.
        POINT p = {};
        if (GetCursorPos(&p) && hwnd != nullptr && ScreenToClient(hwnd, &p))
        {
            io.AddMousePosEvent(static_cast<float>(p.x), static_cast<float>(p.y));
        }

        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);

        feed_overlay_keyboard(hwnd);
    }

    // The keyboard, polled the same way as the mouse rather than taken from window messages. A
    // game reading its keyboard through DirectInput (Bully, for one) may never let a keystroke
    // become a window message at all, so nothing typed reached the panel's search box however the
    // messages were handled. Polling sees every key in every game, and the messages, which are
    // still kept from the game, are no longer fed to ImGui, so nothing is ever typed twice.
    static bool s_key_held[256] = {};
    static ULONGLONG s_key_repeat_at[256] = {};

    void TextureToolkitUI::feed_overlay_keyboard(HWND hwnd)
    {
        ImGuiIO &io = ImGui::GetIO();

        // Callers hold the g_inside_imgui_render exemption, so these read the real keyboard.
        BYTE state[256] = {};
        for (int vk = 1; vk < 255; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000)
                state[vk] = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1)
            state[VK_CAPITAL] |= 1;

        const bool ctrl = (state[VK_CONTROL] & 0x80) != 0;
        const bool alt = (state[VK_MENU] & 0x80) != 0;
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, (state[VK_SHIFT] & 0x80) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, alt);
        io.AddKeyEvent(ImGuiMod_Super, ((state[VK_LWIN] | state[VK_RWIN]) & 0x80) != 0);

        const HKL layout = GetKeyboardLayout(hwnd != nullptr ? GetWindowThreadProcessId(hwnd, nullptr) : 0);
        const ULONGLONG now = GetTickCount64();
        constexpr ULONGLONG kRepeatDelayMs = 400, kRepeatRateMs = 35;

        // Scan codes and ImGui key names depend only on the layout, so they are looked up once per
        // layout rather than a few hundred calls into user32 every frame.
        static HKL s_layout = nullptr;
        static UINT s_scan[256] = {};
        static ImGuiKey s_imgui_key[256] = {};
        if (layout != s_layout)
        {
            s_layout = layout;
            for (int vk = 1; vk < 255; ++vk)
            {
                s_scan[vk] = MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC, layout);
                s_imgui_key[vk] = ImGui_ImplWin32_KeyEventToImGuiKey(static_cast<WPARAM>(vk), static_cast<LPARAM>(s_scan[vk]) << 16);
            }
        }

        for (int vk = 1; vk < 255; ++vk)
        {
            const bool down = (state[vk] & 0x80) != 0;
            const UINT scan = s_scan[vk];

            // Named keys (arrows, Backspace, Enter, Ctrl+A and the like), told to ImGui only when
            // they change; ImGui repeats held ones itself.
            if (down != s_key_held[vk] && s_imgui_key[vk] != ImGuiKey_None)
                io.AddKeyEvent(s_imgui_key[vk], down);

            // Characters, with our own repeat, since nothing here is a message Windows repeats.
            bool emit = false;
            if (down && !s_key_held[vk])
            {
                emit = true;
                s_key_repeat_at[vk] = now + kRepeatDelayMs;
            }
            else if (down && now >= s_key_repeat_at[vk])
            {
                emit = true;
                s_key_repeat_at[vk] = now + kRepeatRateMs;
            }
            s_key_held[vk] = down;

            // Ctrl without Alt is a shortcut, not text; Ctrl+Alt is AltGr on many layouts and is.
            if (!emit || (ctrl && !alt))
                continue;
            wchar_t chars[8] = {};
            // Flag 0x4: leave the keyboard's dead-key state alone, so the game's own typing is
            // not disturbed by our looking.
            const int n = ToUnicodeEx(static_cast<UINT>(vk), scan, state, chars, 8, 0x4, layout);
            for (int i = 0; i < n; ++i)
                if (chars[i] >= 0x20 && chars[i] != 0x7F)
                    io.AddInputCharacterUTF16(static_cast<ImWchar16>(chars[i]));
        }
    }

    // -------------------------------------------------------------------------------------------
    // Status presentation
    // -------------------------------------------------------------------------------------------
    static const ImVec4 &status_color(TextureStatus s)
    {
        if (s == TextureStatus::INJECTED) return pal().ok;
        if (s == TextureStatus::DUMPED)   return pal().info;
        if (s == TextureStatus::PENDING)  return pal().warn;
        if (s == TextureStatus::FAILED)   return pal().bad;
        return pal().neutral;
    }

    // Takes the texture rather than the bare status, so an SK-named replacement can say so: that
    // is the difference between "my file loaded" and "the Special K pack I dropped in loaded".
    static const char *status_label(const TextureDetails &tex)
    {
        if (tex.status == TextureStatus::INJECTED)
            return tex.injected_via_sk_name ? "SK Injected" : "Injected";
        const TextureStatus s = tex.status;
        if (s == TextureStatus::DUMPED)   return "Dumped";
        if (s == TextureStatus::PENDING)  return "Pending";
        if (s == TextureStatus::FAILED)   return "Failed";
        return "Original";
    }

    static int status_rank(const TextureDetails &tex)
    {
        switch (tex.status)
        {
        case TextureStatus::INJECTED: return 0;
        case TextureStatus::FAILED:   return 1;
        case TextureStatus::PENDING:  return 2;
        case TextureStatus::DUMPED:   return 3;
        default:                      return 4;
        }
    }

    // format_short is "DX11_"/"D3D9_" + name; the list shows the name part only.
    static const char *format_name(const TextureDetails &tex)
    {
        const size_t us = tex.format_short.find('_');
        return us == std::string::npos ? tex.format_short.c_str() : tex.format_short.c_str() + us + 1;
    }

    // The format the way a modder says it: RGBA8, BC3, BC7 sRGB, DXT5. The DXGI spelling
    // (R8G8B8A8_UNORM_SRGB) is precise but long enough to push everything else out of the row, and
    // the inspector still shows it in full. Anything without a shorter form passes through as is.
    static std::string compact_format(const char *name)
    {
        static const std::pair<const char *, const char *> kChannels[] = {
            { "R32G32B32A32", "RGBA32" }, { "R16G16B16A16", "RGBA16" }, { "A16B16G16R16F", "ABGR16F" },
            { "A16B16G16R16", "ABGR16" }, { "R10G10B10A2", "RGB10A2" }, { "R32G32B32", "RGB32" },
            { "R8G8B8A8", "RGBA8" },      { "B8G8R8A8", "BGRA8" },      { "B8G8R8X8", "BGRX8" },
            { "A8R8G8B8", "ARGB8" },      { "X8R8G8B8", "XRGB8" },      { "A8B8G8R8", "ABGR8" },
            { "A4R4G4B4", "ARGB4" },      { "A1R5G5B5", "ARGB1555" },   { "X1R5G5B5", "XRGB1555" },
            { "R5G6B5", "RGB565" },       { "R32G32", "RG32" },         { "R16G16", "RG16" },
            { "R8G8", "RG8" },
        };
        static const std::pair<const char *, const char *> kSuffixes[] = {
            { "_UNORM_SRGB", " sRGB" }, { "_UNORM", "" },    { "_SNORM", " snorm" }, { "_TYPELESS", " typeless" },
            { "_FLOAT", "F" },          { "_UINT", " uint" }, { "_SINT", " sint" },
        };

        std::string out = name;
        for (const auto &c : kChannels)
        {
            const size_t n = std::strlen(c.first);
            if (out.compare(0, n, c.first) == 0)
            {
                out = std::string(c.second) + out.substr(n);
                break;
            }
        }
        for (const auto &sfx : kSuffixes)
        {
            const size_t at = out.find(sfx.first);
            if (at != std::string::npos)
            {
                out.replace(at, std::strlen(sfx.first), sfx.second);
                break;
            }
        }
        return out;
    }

    static const char *graphics_api_name()
    {
        if (D3D11Hook::get().get_device() != nullptr) return "Direct3D 11";
        if (D3D9Hook::get().get_device() != nullptr)  return "Direct3D 9";
        return "Waiting for the game";
    }

    // -------------------------------------------------------------------------------------------
    // Shared state: the texture snapshot and settings persistence
    // -------------------------------------------------------------------------------------------
    struct Snapshot
    {
        std::vector<TextureDetails> textures;
        size_t hidden = 0;
        size_t injected = 0, pending = 0, dumped = 0, original = 0;
        uint64_t bytes = 0;
    };

    // Snapshotted at a fixed rate rather than every frame. get_active_textures walks the whole
    // tracked map under the manager lock and copies every row, and a TextureDetails carries ten
    // std::strings. With the 2615 textures a Saints Row 2 session reaches, that is tens of
    // thousands of allocations per frame while holding the lock every texture upload also needs
    // -- the game stopped dead the moment the panel was opened during a load. A texture list does
    // not need to be rebuilt sixty times a second.
    static Snapshot &snapshot(TextureManager &tm)
    {
        static Snapshot s;
        static double s_time = -1.0;
        const double now = ImGui::GetTime();
        if (s_time < 0.0 || (now - s_time) >= 0.25 || s_force_refresh)
        {
            s.textures = tm.get_active_textures(&s.hidden);
            s.injected = s.pending = s.dumped = s.original = 0;
            s.bytes = 0;
            for (const auto &t : s.textures)
            {
                if (t.status == TextureStatus::INJECTED) s.injected++;
                else if (t.status == TextureStatus::PENDING || t.status == TextureStatus::FAILED) s.pending++;
                else if (t.status == TextureStatus::DUMPED) s.dumped++;
                else s.original++;
                s.bytes += t.data_size;
            }
            s_time = now;
            s_force_refresh = false;
        }
        return s;
    }

    // Toggling any option writes it back to the ini at once.
    static void persist_settings(TextureManager &tm)
    {
        Configuration &cfg = ConfigManager::get().get_config();
        cfg.enable_injection = tm.enable_injection;
        cfg.auto_dump = tm.auto_dump;
        cfg.filter_small_textures = tm.filter_small_textures;
        cfg.show_current_frame_only = tm.show_current_frame_only;
        cfg.accept_sk_names = tm.accept_sk_names;
        cfg.highlight_selected = tm.highlight_selected;
        ConfigManager::get().save();
        s_force_refresh = true;
    }

    // -------------------------------------------------------------------------------------------
    // Small layout helpers
    // -------------------------------------------------------------------------------------------
    // `right_reserve` keeps the subtitle clear of controls drawn at the right of the header.
    static void PageHeader(const char *title, const char *subtitle, float right_reserve = 0.0f)
    {
        ImGui::PushFont(font_strong(), kSizeTitle);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::PushFont(nullptr, kSizeSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, pal().text_muted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - right_reserve);
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Width of a row of UI::Buttons, so a group can be right-aligned before it is drawn.
    static float buttons_width(std::initializer_list<const char *> labels)
    {
        float w = 0.0f;
        ImGui::PushFont(font_strong(), 0.0f);
        for (const char *l : labels)
            w += ImGui::CalcTextSize(l, nullptr, true).x + 28.0f;
        ImGui::PopFont();
        return w + ImGui::GetStyle().ItemSpacing.x * static_cast<float>(labels.size() - 1);
    }

    static void CenteredMessage(Icon icon, const char *title, const char *detail)
    {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float cx = origin.x + avail.x * 0.5f;
        const float cy = origin.y + avail.y * 0.42f;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(cx, cy - 34.0f), 22.0f, u32(pal().accent, 0.10f));
        draw_icon(dl, icon, ImVec2(cx, cy - 34.0f), 20.0f, u32(pal().accent));

        const ImVec2 ts = font_strong()->CalcTextSizeA(kSizeHeading, FLT_MAX, 0.0f, title);
        dl->AddText(font_strong(), kSizeHeading, ImVec2(cx - ts.x * 0.5f, cy), u32(pal().text), title);
        if (detail != nullptr)
        {
            const float wrap = (std::min)(avail.x - 40.0f, 340.0f);
            const ImVec2 ds = font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, wrap, detail);
            dl->AddText(font_body(), kSizeSmall, ImVec2(cx - ds.x * 0.5f, cy + ts.y + 6.0f), u32(pal().text_muted),
                        detail, nullptr, wrap);
        }
        ImGui::Dummy(avail);
    }

    // Text in a table cell, centred vertically in a row taller than one line.
    static void CellText(const char *text, float row_inner_h, ImFont *font = nullptr, const ImVec4 *color = nullptr)
    {
        if (font == nullptr)
            font = font_body();
        const float size = (font == font_mono()) ? kSizeMono : kSizeBody;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float th = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).y;
        ImGui::GetWindowDrawList()->AddText(font, size, ImVec2(p.x, p.y + (row_inner_h - th) * 0.5f),
                                            u32(color != nullptr ? *color : pal().text), text);
    }

    // -------------------------------------------------------------------------------------------
    // Inspector
    // -------------------------------------------------------------------------------------------
    // How the inspector's preview is shown, kept as the selection changes.
    static bool s_flip_v = false;
    static bool s_flip_h = false;

    static void DrawPreview(TextureManager &tm, const TextureDetails &tex)
    {
        // One preview, chosen by what is available and useful:
        //   injected  -> the replacement we injected
        //   in scene  -> the live original the game is binding
        //   dumped    -> the dumped .dds loaded from disk
        uint64_t handle = 0;
        const char *source = nullptr;
        uint32_t pw = tex.width, ph = tex.height;
        if (tex.replacement_handle != 0)
        {
            handle = tex.replacement_handle;
            source = "Replacement";
            pw = tex.repl_width;
            ph = tex.repl_height;
        }
        else if ((handle = tm.get_original_preview_handle()) != 0)
        {
            source = "Live original";
        }
        else if (!tex.filepath_dumped.empty())
        {
            handle = tm.get_file_preview_handle(tex.hash, tex.filepath_dumped, tex.is_dx11);
            if (handle != 0)
                source = "From dump";
        }

        const float box_w = ImGui::GetContentRegionAvail().x;
        const float box_h = (std::min)(box_w, 260.0f);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + box_w, a.y + box_h);
        ImGui::Dummy(ImVec2(box_w, box_h));

        ImDrawList *dl = ImGui::GetWindowDrawList();
        Checkerboard(dl, a, b, 10.0f, 9.0f);

        if (handle != 0 && pw > 0 && ph > 0)
        {
            // Fit, keep the aspect, and centre in the box.
            const float pad = 12.0f;
            const float aw = box_w - pad * 2.0f, ah = box_h - pad * 2.0f;
            const float scale = (std::min)(aw / static_cast<float>(pw), ah / static_cast<float>(ph));
            const float iw = static_cast<float>(pw) * scale, ih = static_cast<float>(ph) * scale;
            const ImVec2 i0(a.x + (box_w - iw) * 0.5f, a.y + (box_h - ih) * 0.5f);
            // Flipping swaps the texture coordinates: the preview only, never the texture.
            const ImVec2 uv0(s_flip_h ? 1.0f : 0.0f, s_flip_v ? 1.0f : 0.0f);
            const ImVec2 uv1(s_flip_h ? 0.0f : 1.0f, s_flip_v ? 0.0f : 1.0f);
            dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(handle)), i0, ImVec2(i0.x + iw, i0.y + ih),
                                uv0, uv1, IM_COL32_WHITE, 4.0f);
        }
        else
        {
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            dl->AddRectFilled(a, b, u32(pal().surface_sunken, 0.55f), 9.0f);
            draw_icon(dl, Icon::Grid, ImVec2(c.x, c.y - 12.0f), 22.0f, u32(pal().text_faint));
            const char *msg = "Not on screen, and not dumped";
            const ImVec2 ts = font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, msg);
            dl->AddText(font_body(), kSizeSmall, ImVec2(c.x - ts.x * 0.5f, c.y + 12.0f), u32(pal().text_muted), msg);
        }

        dl->AddRect(a, b, u32(pal().border), 9.0f, 0, 1.0f);

        // Where the picture came from, and its size, as chips on the image.
        if (source != nullptr)
        {
            const ImVec2 ss = font_strong()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, source);
            const ImVec2 c0(a.x + 10.0f, a.y + 10.0f), c1(c0.x + ss.x + 16.0f, c0.y + ss.y + 6.0f);
            dl->AddRectFilled(c0, c1, u32(ImVec4(0.059f, 0.055f, 0.051f, 0.80f)), 999.0f);
            dl->AddText(font_strong(), kSizeSmall, ImVec2(c0.x + 8.0f, c0.y + 3.0f), u32(pal().text), source);

            char dims[32];
            std::snprintf(dims, sizeof(dims), "%u x %u", pw, ph);
            const ImVec2 ds = font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, dims);
            const ImVec2 d1(b.x - 10.0f, a.y + 10.0f + ds.y + 6.0f), d0(d1.x - ds.x - 16.0f, a.y + 10.0f);
            dl->AddRectFilled(d0, d1, u32(ImVec4(0.059f, 0.055f, 0.051f, 0.80f)), 999.0f);
            dl->AddText(font_body(), kSizeSmall, ImVec2(d0.x + 8.0f, d0.y + 3.0f), u32(pal().text_muted), dims);

            // Flip chips, bottom left, for art a game stores upside down or mirrored.
            const ImVec2 after = ImGui::GetCursorScreenPos();
            float x = a.x + 10.0f;
            const auto chip = [&](const char *id, const char *label, bool *flag, const char *tip)
            {
                const ImVec2 ts = font_strong()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, label);
                const ImVec2 c0(x, b.y - 10.0f - ts.y - 6.0f), c1(c0.x + ts.x + 16.0f, b.y - 10.0f);
                ImGui::SetCursorScreenPos(c0);
                if (ImGui::InvisibleButton(id, ImVec2(c1.x - c0.x, c1.y - c0.y)))
                    *flag = !*flag;
                ImGui::SetItemTooltip("%s", tip);
                const bool hot = ImGui::IsItemHovered();
                dl->AddRectFilled(c0, c1, *flag ? u32(pal().accent_fill)
                                                : u32(ImVec4(0.059f, 0.055f, 0.051f, hot ? 0.95f : 0.80f)), 999.0f);
                dl->AddText(font_strong(), kSizeSmall, ImVec2(c0.x + 8.0f, c0.y + 3.0f),
                            u32(*flag ? pal().accent_text : pal().text_muted), label);
                x = c1.x + 6.0f;
            };
            chip("##flip_v", "Flip V", &s_flip_v, "Show the preview upside down. Only the preview; the texture is untouched.");
            chip("##flip_h", "Flip H", &s_flip_h, "Show the preview mirrored. Only the preview; the texture is untouched.");
            ImGui::SetCursorScreenPos(after);
        }
    }

    static void DrawInspector(TextureManager &tm, const TextureDetails &tex)
    {
        DrawPreview(tm, tex);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        const std::string hash = "0x" + tex.hash_hex;
        ImGui::PushFont(font_mono(), kSizeMono + 2.0f); // the selected hash, as the inspector's heading
        ImGui::TextUnformatted(hash.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        {
            const ImVec2 ps = PillSize(status_label(tex));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - ps.y) * 0.5f + 2.0f);
            Pill(status_label(tex), status_color(tex.status));
        }

        if (UI::Button("Copy hash"))
        {
            ImGui::SetClipboardText(hash.c_str());
            SetStatusMessage("Copied " + hash + " to the clipboard.");
        }
        ImGui::SetItemTooltip("Copy the hash. Name your replacement %s.dds and put it in TT/inject.", tex.hash_hex.c_str());
        ImGui::SameLine();
        if (UI::Button("Dump"))
        {
            s_force_refresh = true;
            if (tm.request_dump(tex.hash))
                SetStatusMessage("Dumped " + hash + " to TT/dump.");
            else
                SetStatusMessage("Could not dump " + hash + ". For default-pool textures, turn on Auto-dump (see log).");
        }
        ImGui::SetItemTooltip("Write this texture to TT/dump as a .dds, with its full mip chain.");

        // Only offered when there is a dump to delete, and asked once more: it removes a file.
        if (!tex.filepath_dumped.empty())
        {
            ImGui::SameLine();
            if (UI::Button("Delete dump", ButtonKind::Ghost))
                ImGui::OpenPopup("##confirm_delete_dump");
            ImGui::SetItemTooltip("Delete this texture's .dds from TT/dump.");
            if (ImGui::BeginPopup("##confirm_delete_dump"))
            {
                ImGui::TextUnformatted("Delete this texture's dump from TT/dump?");
                ImGui::PushFont(nullptr, kSizeSmall);
                ImGui::TextColored(pal().text_faint, "%s", tex.filepath_dumped.c_str());
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                if (UI::Button("Delete", ButtonKind::Primary))
                {
                    s_force_refresh = true;
                    SetStatusMessage(tm.delete_dump(tex.hash) ? "Deleted the dump of " + hash + "."
                                                              : "Could not delete the dump of " + hash + " (see log).");
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (UI::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        // Blink this texture in the game, to find what it is drawn on. On a line of its own, as
        // the inspector can be too narrow for it beside the buttons.
        {
            const ImVec2 sw = ToggleSwitchSize();
            const float row_h = ImGui::GetTextLineHeight() + 4.0f;
            const float y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(y + (row_h - sw.y) * 0.5f);
            const char *tip = "Make this texture blink magenta in the game while it is selected here,\n"
                              "so whatever it is drawn on can be found by eye.";
            if (ToggleSwitch("##blink", &tm.highlight_selected))
                persist_settings(tm);
            ImGui::SetItemTooltip("%s", tip);
            ImGui::SameLine();
            ImGui::SetCursorPosY(y + (row_h - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextUnformatted("Blink in game");
            if (ImGui::IsItemClicked())
            {
                tm.highlight_selected = !tm.highlight_selected;
                persist_settings(tm);
            }
            ImGui::SetItemTooltip("%s", tip);
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        SectionLabel("DETAILS");
        ImGui::Dummy(ImVec2(0.0f, 1.0f));

        char buf[160];
        std::snprintf(buf, sizeof(buf), "%u x %u", tex.width, tex.height);
        KeyValue("Dimensions", buf);
        if (tex.replacement_handle != 0)
        {
            std::snprintf(buf, sizeof(buf), "%u x %u", tex.repl_width, tex.repl_height);
            KeyValue("Replacement", buf, false, &pal().ok);
        }
        std::snprintf(buf, sizeof(buf), "%u", tex.mip_levels);
        KeyValue("Mip levels", buf);
        std::snprintf(buf, sizeof(buf), "%.3f MiB", tex.data_size / (1024.0 * 1024.0));
        KeyValue("Data size", buf);
        // As export tools spell it (BC7_UNORM, R8G8B8A8_UNORM), without the API prefix.
        std::snprintf(buf, sizeof(buf), "%s  (%u)", format_name(tex), tex.format_id);
        KeyValue("Format", buf);
        if (tex.view_format_id != 0 && tex.view_format_id != tex.format_id)
        {
            std::snprintf(buf, sizeof(buf), "%s  (%u)", tex.view_format_str.c_str(), tex.view_format_id);
            KeyValue("Sampled as", buf);
        }
        KeyValue("Compressed", tex.is_compressed ? "Yes" : "No");
        KeyValue("sRGB", tex.is_srgb ? "Yes" : "No");

        if (tex.is_dx11)
        {
            std::snprintf(buf, sizeof(buf), "%u", tex.array_size);
            KeyValue("Array size", buf);
            std::snprintf(buf, sizeof(buf), "0x%04X", tex.bind_flags);
            KeyValue("Bind flags", buf, true);
            std::snprintf(buf, sizeof(buf), "%u", tex.usage);
            KeyValue("Usage", buf);
            std::snprintf(buf, sizeof(buf), "0x%02X", tex.cpu_access);
            KeyValue("CPU access", buf, true);
            std::snprintf(buf, sizeof(buf), "0x%02X", tex.misc_flags);
            KeyValue("Misc flags", buf, true);
        }

        const std::string *path = nullptr;
        if (tex.status == TextureStatus::INJECTED && !tex.filepath_injected.empty())
            path = &tex.filepath_injected;
        else if (tex.status == TextureStatus::DUMPED && !tex.filepath_dumped.empty())
            path = &tex.filepath_dumped;
        if (path != nullptr)
        {
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            SectionLabel("FILE");
            ImGui::PushFont(nullptr, kSizeSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, pal().text_muted);
            ImGui::TextWrapped("%s", path->c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }

    // -------------------------------------------------------------------------------------------
    // Pages
    // -------------------------------------------------------------------------------------------
    static void DrawTexturesPage(TextureManager &tm)
    {
        Snapshot &snap = snapshot(tm);

        // Header, with dumping on the right: the switch that dumps as textures load, the button
        // that dumps what is listed now, and the folder they go to.
        {
            const ImVec2 sw = ToggleSwitchSize();
            const float label_w = ImGui::CalcTextSize("Auto-dump").x;
            const float icon_btn = 30.0f;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float right = sw.x + 8.0f + label_w + 18.0f + buttons_width({ "Dump all" }) + gap + icon_btn;

            const float start_y = ImGui::GetCursorPosY();
            const float right_edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kCloseReserve;
            PageHeader("Textures", "Everything the game has uploaded. Select one to inspect, dump or replace it.",
                       right + kCloseReserve + 16.0f);
            const float end_y = ImGui::GetCursorPosY();

            const float row_y = start_y + 6.0f;
            const float button_h = ImGui::GetFontSize() + 14.0f;
            float x = right_edge - right;

            ImGui::SetCursorPos(ImVec2(x, row_y + (button_h - sw.y) * 0.5f));
            if (ToggleSwitch("##auto_dump", &tm.auto_dump))
                persist_settings(tm);
            ImGui::SetItemTooltip("Save every texture to TT/dump as it loads.\nSlows loading, and fills the folder quickly.");
            x += sw.x + 8.0f;
            ImGui::SetCursorPos(ImVec2(x, row_y + (button_h - ImGui::GetTextLineHeight()) * 0.5f));
            ImGui::TextUnformatted("Auto-dump");
            if (ImGui::IsItemClicked())
            {
                tm.auto_dump = !tm.auto_dump;
                persist_settings(tm);
            }
            ImGui::SetItemTooltip("Save every texture to TT/dump as it loads.\nSlows loading, and fills the folder quickly.");
            x += label_w + 18.0f;

            ImGui::SetCursorPos(ImVec2(x, row_y));
            if (UI::Button("Dump all", ButtonKind::Primary))
            {
                const size_t n = tm.dump_all(tm.show_current_frame_only);
                s_force_refresh = true;
                SetStatusMessage("Dump all queued " + std::to_string(n) + (tm.show_current_frame_only ? " on-screen" : " tracked") +
                                 " texture(s); each is written the next time it is drawn.");
            }
            ImGui::SetItemTooltip("Dump every listed texture: only the current scene while \"Current scene only\" is on.\n"
                                  "Each is written to TT/dump the next time it is drawn.");
            ImGui::SameLine(0.0f, gap);
            ImGui::SetCursorPosY(row_y + (button_h - icon_btn) * 0.5f);
            if (IconButton("##open_dump", Icon::Folder, "Open TT/dump", icon_btn))
                OpenDirectory(tm.get_dump_dir());

            ImGui::SetCursorPosY((std::max)(end_y, row_y + button_h + 4.0f));
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        // Figures.
        {
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float tile_w = (ImGui::GetContentRegionAvail().x - gap * 4.0f) / 5.0f;
            char v[32];

            std::snprintf(v, sizeof(v), "%zu", snap.textures.size());
            StatTile(tm.show_current_frame_only ? "On screen" : "Tracked", v, pal().accent, tile_w,
                     tm.show_current_frame_only ? "Textures drawn in the current scene." : "Every texture tracked this session.");
            ImGui::SameLine();
            std::snprintf(v, sizeof(v), "%zu", snap.injected);
            StatTile("Injected", v, pal().ok, tile_w, "Replaced by a file from TT/inject, and on screen now.");
            ImGui::SameLine();
            std::snprintf(v, sizeof(v), "%zu", snap.pending);
            StatTile("Not applied", v, pal().warn, tile_w,
                     "An inject file exists for these, but no replacement is on screen.\n"
                     "Pending ones apply the next time the texture is drawn.\n"
                     "Failed ones were refused: the log says why. Fix the file, then Reload.");
            ImGui::SameLine();
            std::snprintf(v, sizeof(v), "%zu", snap.dumped);
            StatTile("Dumped", v, pal().info, tile_w, "Written to TT/dump.");
            ImGui::SameLine();
            std::snprintf(v, sizeof(v), "%.1f MiB", snap.bytes / (1024.0 * 1024.0));
            StatTile("Texture memory", v, pal().neutral, tile_w, "GPU size of the listed textures, full mip chains included.");
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        // Search, the scene filter, and how many textures that filter is hiding.
        {
            const float search_w = (std::min)(ImGui::GetContentRegionAvail().x * 0.34f, 320.0f);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32.0f, 7.0f));
            ImGui::SetNextItemWidth(search_w);
            ImGui::InputTextWithHint("##filter", "Search by hash, size or format", s_filter_buf, sizeof(s_filter_buf));
            ImGui::PopStyleVar();
            draw_icon(ImGui::GetWindowDrawList(), Icon::Search,
                      ImVec2(sp.x + 17.0f, sp.y + ImGui::GetItemRectSize().y * 0.5f), 13.0f, u32(pal().text_muted));

            ImGui::SameLine(0.0f, 18.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
            if (ToggleSwitch("##scene_only", &tm.show_current_frame_only))
                persist_settings(tm);
            ImGui::SetItemTooltip("List only textures drawn in the current scene.");
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Current scene only");
            if (ImGui::IsItemClicked())
            {
                tm.show_current_frame_only = !tm.show_current_frame_only;
                persist_settings(tm);
            }
            ImGui::SetItemTooltip("List only textures drawn in the current scene.");

            ImGui::SameLine(0.0f, 18.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
            if (ToggleSwitch("##skip_small", &tm.filter_small_textures))
                persist_settings(tm);
            ImGui::SetItemTooltip("Ignore textures under 16 x 16: lookup tables and placeholders,\nrarely worth replacing, that crowd the list.\nSwitched off, tiny textures are listed as they next load.");
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Skip under 16 x 16");
            if (ImGui::IsItemClicked())
            {
                tm.filter_small_textures = !tm.filter_small_textures;
                persist_settings(tm);
            }
            ImGui::SetItemTooltip("Ignore textures under 16 x 16: lookup tables and placeholders,\nrarely worth replacing, that crowd the list.\nSwitched off, tiny textures are listed as they next load.");

            // A texture the game uploads but never draws with is tracked and then filtered
            // straight back out, which reads as "the tool cannot see it" when the truth is that
            // the list is hiding it. Say how many, and make showing them one click.
            if (tm.show_current_frame_only && snap.hidden > 0)
            {
                ImGui::SameLine(0.0f, 14.0f);
                char label[64];
                std::snprintf(label, sizeof(label), "%zu hidden  \xC2\xB7  Show all", snap.hidden);
                ImGui::PushStyleColor(ImGuiCol_Text, pal().warn);
                if (UI::Button(label, ButtonKind::Ghost))
                {
                    tm.show_current_frame_only = false;
                    persist_settings(tm);
                    SetStatusMessage("Listing every tracked texture, including ones not drawn right now.");
                }
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Tracked, but not drawn in the current scene.\n"
                                      "Textures the game uploads without ever drawing them, such as art\n"
                                      "composited into a render target, live here.");
            }
        }

        std::string filter = s_filter_buf;
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        // The filtered, sorted view, built once so [ and ] can step through it by index.
        std::vector<const TextureDetails *> shown;
        shown.reserve(snap.textures.size());
        for (const auto &tex : snap.textures)
        {
            if (!filter.empty())
            {
                std::string h = tex.hash_hex;
                std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                const std::string dim = std::to_string(tex.width) + "x" + std::to_string(tex.height);
                std::string fmt = tex.format_short + " " + compact_format(format_name(tex));
                std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::tolower);
                if (h.find(filter) == std::string::npos && dim.find(filter) == std::string::npos &&
                    fmt.find(filter) == std::string::npos)
                    continue;
            }
            shown.push_back(&tex);
        }

        // Sort by whatever column was clicked last. Applied here, ahead of the table, so the
        // keyboard stepping below walks the list in the order it is drawn.
        static int s_sort_column = 4;
        static bool s_sort_ascending = true;
        std::stable_sort(shown.begin(), shown.end(), [](const TextureDetails *a, const TextureDetails *b) {
            int c = 0;
            switch (s_sort_column)
            {
            case 0: c = (a->hash < b->hash) ? -1 : (a->hash > b->hash ? 1 : 0); break;
            case 1:
            {
                const uint64_t aa = static_cast<uint64_t>(a->width) * a->height, bb = static_cast<uint64_t>(b->width) * b->height;
                c = (aa < bb) ? -1 : (aa > bb ? 1 : 0);
                break;
            }
            case 2: c = static_cast<int>(a->mip_levels) - static_cast<int>(b->mip_levels); break;
            case 3: c = compact_format(format_name(*a)).compare(compact_format(format_name(*b))); break;
            default: c = status_rank(*a) - status_rank(*b); break;
            }
            return s_sort_ascending ? (c < 0) : (c > 0);
        });

        // Two resizable panes: list on the left, inspector on the right. Drag the gap between
        // them to change the split.
        static float s_split = 0.60f;
        const float splitter_w = 10.0f;
        const float total_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y;
        const float list_w = (total_w - splitter_w) * s_split;

        static bool s_scroll_to_sel = false;
        bool list_hovered = false;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal().surface_sunken);
        ImGui::BeginChild("list", ImVec2(list_w, avail_h), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();
        {
            // True whenever the mouse is over the list. ChildWindows is required because a
            // ScrollY table creates its own inner scroll window, so hovering a row makes that
            // inner window (not this one) the hovered window.
            list_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

            if (snap.textures.empty())
            {
                CenteredMessage(Icon::Grid, tm.show_current_frame_only && snap.hidden > 0 ? "Nothing on screen right now" : "No textures yet",
                                tm.show_current_frame_only && snap.hidden > 0
                                    ? "Textures are tracked but none is being drawn. Turn off \"Current scene only\" to list them."
                                    : "Textures appear here as the game uploads them. Play for a moment, then look again.");
            }
            else if (shown.empty())
            {
                CenteredMessage(Icon::Search, "No matches", "Nothing matches that search. Try part of a hash, a size like 512x512, or a format like BC3.");
            }
            else
            {
                ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable | ImGuiTableFlags_PadOuterX;
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 3.0f));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(pal().accent.x, pal().accent.y, pal().accent.z, 0.24f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(pal().accent.x, pal().accent.y, pal().accent.z, 0.10f));
                if (ImGui::BeginTable("textures", 5, flags))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 128.0f);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                    ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 46.0f);
                    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 84.0f);
                    ImGui::TableHeadersRow();

                    if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs())
                    {
                        if (specs->SpecsDirty && specs->SpecsCount > 0)
                        {
                            s_sort_column = specs->Specs[0].ColumnIndex;
                            s_sort_ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                            specs->SpecsDirty = false;
                        }
                    }

                    // A Selectable grows by the item spacing to close the gaps between rows, so it
                    // is sized short by that much to land exactly on the row's edges.
                    const float row_h = 30.0f;
                    const float inner_h = row_h - ImGui::GetStyle().ItemSpacing.y;

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(shown.size()), row_h);
                    // Keep the selected row inside the clipped range when it is about to be
                    // scrolled into view, so the scroll request reaches it.
                    if (s_scroll_to_sel)
                    {
                        for (int i = 0; i < static_cast<int>(shown.size()); ++i)
                            if (shown[i]->hash == s_selected_texture_hash) { clipper.IncludeItemByIndex(i); break; }
                    }

                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const TextureDetails &tex = *shown[i];
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
                            ImGui::PushID(static_cast<int>(tex.hash ^ (tex.hash >> 32)));

                            ImGui::TableSetColumnIndex(0);
                            const bool selected = (s_selected_texture_hash == tex.hash);
                            const ImVec2 cell = ImGui::GetCursorScreenPos();
                            if (ImGui::Selectable("##row", selected,
                                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                                  ImVec2(0.0f, inner_h)))
                                s_selected_texture_hash = tex.hash;
                            if (selected && s_scroll_to_sel)
                            {
                                ImGui::SetScrollHereY(0.5f);
                                s_scroll_to_sel = false;
                            }
                            if (selected)
                                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cell.x - 6.0f, cell.y + 3.0f),
                                                                          ImVec2(cell.x - 3.0f, cell.y + inner_h - 3.0f),
                                                                          u32(pal().accent), 2.0f);
                            {
                                // No 0x here: it is the same on every row, and the width goes to the
                                // format column instead. The inspector, and Copy hash, keep it.
                                const std::string &h = tex.hash_hex;
                                const float th = font_mono()->CalcTextSizeA(kSizeMono, FLT_MAX, 0.0f, h.c_str()).y;
                                ImGui::GetWindowDrawList()->AddText(font_mono(), kSizeMono,
                                                                    ImVec2(cell.x, cell.y + (inner_h - th) * 0.5f),
                                                                    u32(selected ? pal().text : pal().text_muted), h.c_str());
                            }

                            char buf[32];
                            ImGui::TableSetColumnIndex(1);
                            std::snprintf(buf, sizeof(buf), "%u x %u", tex.width, tex.height);
                            CellText(buf, inner_h);

                            ImGui::TableSetColumnIndex(2);
                            std::snprintf(buf, sizeof(buf), "%u", tex.mip_levels);
                            CellText(buf, inner_h, nullptr, &pal().text_muted);

                            ImGui::TableSetColumnIndex(3);
                            CellText(compact_format(format_name(tex)).c_str(), inner_h);

                            ImGui::TableSetColumnIndex(4);
                            {
                                const char *label = status_label(tex);
                                const ImVec2 ps = PillSize(label);
                                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (inner_h - ps.y) * 0.5f);
                                Pill(label, status_color(tex.status));
                            }

                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }
        }
        ImGui::EndChild();

        // Step through the list with [ and ] while it is hovered. The keys are fed into ImGui by
        // feed_overlay_mouse, which polls them, so this works under exclusive-DirectInput games
        // that post no key messages.
        if (list_hovered && !shown.empty())
            ImGui::SetItemTooltip("Press [ or ] to step through textures.");
        {
            const bool go_prev = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false);
            const bool go_next = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false);
            // Not while typing: [ and ] are characters in the search box.
            const int dir = (list_hovered && !ImGui::GetIO().WantTextInput) ? (go_prev ? -1 : (go_next ? 1 : 0)) : 0;

            if (dir != 0 && !shown.empty())
            {
                int cur = -1;
                for (int i = 0; i < static_cast<int>(shown.size()); ++i)
                    if (shown[i]->hash == s_selected_texture_hash) { cur = i; break; }

                int next = (cur < 0) ? (dir > 0 ? 0 : static_cast<int>(shown.size()) - 1) : cur + dir;
                next = (std::max)(0, (std::min)(next, static_cast<int>(shown.size()) - 1));
                s_selected_texture_hash = shown[next]->hash;
                s_scroll_to_sel = true;
            }
        }

        // Draggable divider, with a grip that lights up when it is grabbed.
        ImGui::SameLine(0.0f, 0.0f);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##splitter", ImVec2(splitter_w, avail_h));
            if (ImGui::IsItemActive())
                s_split += ImGui::GetIO().MouseDelta.x / (total_w - splitter_w);
            s_split = (std::max)(0.30f, (std::min)(s_split, 0.75f));
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const float cx = p.x + splitter_w * 0.5f, cy = p.y + avail_h * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - 1.5f, cy - 18.0f), ImVec2(cx + 1.5f, cy + 18.0f),
                                                      u32(hot ? pal().accent : pal().text_faint, hot ? 1.0f : 0.5f), 2.0f);
        }
        ImGui::SameLine(0.0f, 0.0f);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal().surface_raised);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
        ImGui::BeginChild("inspector", ImVec2(0, avail_h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        {
            // Keep the live-preview capture, and the in-game blink, aimed at the current selection.
            tm.set_preview_target(s_selected_texture_hash);
            tm.set_highlight_target(s_selected_texture_hash);

            const TextureDetails *sel = nullptr;
            for (const auto &t : snap.textures)
                if (t.hash == s_selected_texture_hash) { sel = &t; break; }

            if (sel != nullptr)
                DrawInspector(tm, *sel);
            else
                CenteredMessage(Icon::Layers, "Select a texture", "Pick one from the list to preview it and see its details. [ and ] step through the list.");
        }
        ImGui::EndChild();
    }

    // The inject folder and every texture mod, in load order, each with its switch and the
    // buttons that move it up or down. Changes are written to TextureToolkit.ini and applied at
    // once (a rescan, as Reload does).
    static void DrawModsCard(TextureManager &tm)
    {
        BeginCard("mods", "Mods and load order",
                  "Any folder in TT other than dump and inject is a texture mod. Where two ship the same "
                  "texture, the one higher in this list wins. An optional mod.ini in the folder gives it a "
                  "name, author, version and description.");

        const std::vector<TextureManager::ModInfo> mods = tm.get_mods();
        const ImVec2 sw = ToggleSwitchSize();
        const float btn = 26.0f;
        const float gap = 4.0f;

        // Applied after the loop: each one rescans, which replaces the list being drawn.
        enum class Action { None, Toggle, Up, Down } action = Action::None;
        std::wstring action_id;
        bool action_value = false;

        for (size_t i = 0; i < mods.size(); ++i)
        {
            const TextureManager::ModInfo &m = mods[i];
            ImGui::PushID(static_cast<int>(i));

            const float right = CardRightEdge();
            const float controls_w = btn * 3.0f + gap * 2.0f + 14.0f + (m.is_base ? PillSize("Always on").x : sw.x);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const bool live = m.is_base ? tm.enable_injection : m.enabled;
            const ImVec4 &fg = live ? pal().text : pal().text_faint;

            if (i != 0)
            {
                ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, start.y - 4.0f), ImVec2(right, start.y - 4.0f), u32(pal().border));
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
            }
            const ImVec2 row = ImGui::GetCursorScreenPos();

            // Left: priority, name, version, author, description, and what it contributes.
            ImGui::PushTextWrapPos(right - controls_w - 12.0f - ImGui::GetWindowPos().x);
            ImGui::BeginGroup();
            {
                char prio[8];
                std::snprintf(prio, sizeof(prio), "%zu", i + 1);
                ImGui::PushFont(nullptr, kSizeSmall);
                ImGui::TextColored(pal().text_faint, "%s", prio);
                ImGui::PopFont();
                ImGui::SameLine(0.0f, 10.0f);

                ImGui::BeginGroup();
                ImGui::PushFont(font_strong(), 0.0f);
                ImGui::TextColored(fg, "%s", m.is_base ? "TT/inject" : m.name.c_str());
                ImGui::PopFont();
                if (!m.version.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(pal().text_muted, "v%s", m.version.c_str());
                }
                if (!m.author.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(pal().text_muted, "by %s", m.author.c_str());
                }

                ImGui::PushFont(nullptr, kSizeSmall);
                if (!m.description.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, live ? pal().text_muted : pal().text_faint);
                    ImGui::TextWrapped("%s", m.description.c_str());
                    ImGui::PopStyleColor();
                }

                std::string detail;
                if (!live)
                    detail = m.is_base ? "Off while Replace textures is off" : "Off";
                else
                {
                    detail = std::to_string(m.file_count) + (m.file_count == 1 ? " file" : " files");
                    if (m.provided < m.file_count)
                        detail += ", " + std::to_string(m.file_count - m.provided) + " covered by a mod above";
                }
                if (!m.is_base)
                {
                    // The folder, not the display name, is what the ini and the disk know it by.
                    detail += "  \xC2\xB7  TT/" + path_utf8(m.id);
                }
                if (!m.is_base && m.overridden && m.enabled != m.enabled_default)
                    detail += m.enabled_default ? "  \xC2\xB7  on by default" : "  \xC2\xB7  off by default";
                ImGui::PushStyleColor(ImGuiCol_Text, pal().text_faint);
                ImGui::TextWrapped("%s", detail.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::EndGroup();
            }
            ImGui::EndGroup();
            ImGui::PopTextWrapPos();
            const ImVec2 after = ImGui::GetCursorScreenPos();

            // Right: up, down, open folder, then the switch.
            float x = right - controls_w;
            const float y = row.y;
            ImGui::SetCursorScreenPos(ImVec2(x, y - 4.0f));
            ImGui::BeginDisabled(i == 0);
            if (IconButton("##up", Icon::ChevronUp, "Higher priority", btn))
                action = Action::Up, action_id = m.id;
            ImGui::EndDisabled();
            x += btn + gap;
            ImGui::SetCursorScreenPos(ImVec2(x, y - 4.0f));
            ImGui::BeginDisabled(i + 1 == mods.size());
            if (IconButton("##down", Icon::ChevronDown, "Lower priority", btn))
                action = Action::Down, action_id = m.id;
            ImGui::EndDisabled();
            x += btn + gap;
            ImGui::SetCursorScreenPos(ImVec2(x, y - 4.0f));
            if (IconButton("##open", Icon::Folder, "Open this folder", btn))
                OpenDirectory(m.dir);
            x += btn + 14.0f;

            ImGui::SetCursorScreenPos(ImVec2(x, y + (ImGui::GetTextLineHeight() - sw.y) * 0.5f + 1.0f));
            if (m.is_base)
            {
                Pill(tm.enable_injection ? "Always on" : "Off", tm.enable_injection ? pal().ok : pal().neutral);
                ImGui::SetItemTooltip("TT/inject is switched with Replace textures above.");
            }
            else
            {
                bool on = m.enabled;
                if (ToggleSwitch("##on", &on))
                    action = Action::Toggle, action_id = m.id, action_value = on;
                ImGui::SetItemTooltip(m.has_manifest ? (m.enabled_default ? "On by default (mod.ini)" : "Off by default (mod.ini)")
                                                     : "On by default (no mod.ini)");
            }

            ImGui::SetCursorScreenPos(after);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::PopID();
        }

        if (mods.size() <= 1)
        {
            ImGui::PushFont(nullptr, kSizeSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, pal().text_faint);
            ImGui::TextWrapped("No mods installed. Make a folder in TT next to inject, for example TT/DualShock, put "
                               "its .dds files in it (subfolders are fine) and press Reload replacements.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        if (UI::Button("Open TT folder"))
            OpenDirectory(tm.get_resource_root());

        EndCard();

        if (action != Action::None)
        {
            const std::string name = path_utf8(action_id);
            if (action == Action::Toggle)
            {
                tm.set_mod_enabled(action_id, action_value);
                SetStatusMessage(std::string(action_value ? "Switched on " : "Switched off ") + name + ".");
            }
            else
            {
                tm.move_mod(action_id, action == Action::Up ? -1 : 1);
                SetStatusMessage(std::string("Moved ") + name + (action == Action::Up ? " up" : " down") + " the load order.");
            }
            s_force_refresh = true;
        }
    }

    static void DrawModFilesPage(TextureManager &tm)
    {
        PageHeader("Mod files", "Replacement textures: your own in TT/inject, and the mods installed next to it.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        const TextureManager::InjectionStats inj = tm.get_injection_stats();

        BeginCard("inject", "Replacements",
                  "A .dds in TT/inject replaces the texture whose hash it is named after. Files added while the game runs are picked up on Reload.");
        {
            if (ToggleRow("Replace textures", "Swap in a replacement wherever a matching file exists.", &tm.enable_injection))
                persist_settings(tm);
            if (ToggleRow("Accept Special K names",
                          "Also load packs named the way Special K names them: eight hex digits, the CRC-32C of the top mip. These show as SK Injected. Switching it on applies to textures as they load, so restart the game for ones already loaded.",
                          &tm.accept_sk_names))
            {
                persist_settings(tm);
                tm.rescan_injected(); // drop, or pick up, the SK-named files' replacements now
            }

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float card_avail = ImGui::GetContentRegionAvail().x - 16.0f;
            const float tile_w = (card_avail - gap * 2.0f) / 3.0f;
            char v[32];
            std::snprintf(v, sizeof(v), "%zu", inj.files_found);
            StatTile("Files found", v, pal().accent, tile_w, "Distinct textures with a replacement, across TT/inject and every mod that is on.");
            ImGui::SameLine();
            std::snprintf(v, sizeof(v), "%zu", inj.applied);
            StatTile("Applied", v, pal().ok, tile_w, "Replacements built and bound. This lags \"found\" until each texture is next drawn.");
            ImGui::SameLine();
            std::snprintf(v, sizeof(v), "%zu", inj.failed);
            StatTile("Failed", v, inj.failed > 0 ? pal().bad : pal().neutral, tile_w,
                     "Files that could not be loaded or created. The log line for each says why:\n"
                     "an unsupported format, a truncated file, or a mapping that does not exist.");

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (UI::Button("Reload replacements", ButtonKind::Primary))
            {
                tm.rescan_injected();
                s_force_refresh = true;
                SetStatusMessage("Rescanned TT/inject and the mod folders for DDS replacements.");
            }
            ImGui::SetItemTooltip("Rescan TT/inject and every mod folder, pick up new mods, and reload every replacement.");
            ImGui::SameLine();
            if (UI::Button("Open inject folder"))
                OpenDirectory(tm.get_inject_dir());

            ImGui::PushFont(nullptr, kSizeSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, pal().text_faint);
            ImGui::TextWrapped("%s", path_utf8(tm.get_inject_dir()).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        EndCard();

        DrawModsCard(tm);
    }

    static void DrawSettingsPage(TextureManager &tm)
    {
        PageHeader("Settings", "The overlay and where Texture Toolkit keeps its files. Saved to TextureToolkit.ini as soon as they change.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        Configuration &cfg = ConfigManager::get().get_config();
        BeginCard("overlay", "Overlay");
        {
            if (ToggleRow("Show startup banner", "The notice that appears when a game starts, naming the key that opens this panel.",
                          &cfg.show_osd_banner))
            {
                ConfigManager::get().save();
            }
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            KeyValue("Panel key", hotkey_name(cfg.hotkey).c_str(), false, &pal().accent);
            ImGui::PushFont(nullptr, kSizeSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, pal().text_faint);
            ImGui::TextWrapped("Change HotKey in TextureToolkit.ini and restart the game to use a different key, for example 0x24 for Home or 0x74 for F5.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        EndCard();

        BeginCard("folders", "Folders", "Set ResourceRoot in TextureToolkit.ini to move all of these at once.");
        KeyValue("Resource root", path_utf8(tm.get_resource_root()).c_str());
        KeyValue("Inject", path_utf8(tm.get_inject_dir()).c_str());
        KeyValue("Dump", path_utf8(tm.get_dump_dir()).c_str());
        EndCard();
    }

    // ---- Build info ---------------------------------------------------------------------------
    // Everything worth knowing about a user's setup when a texture will not show, on one card that
    // can be screenshotted, or copied as text with one click and pasted into a bug report.

    // GetVersionEx lies to an unmanifested process (every game says Windows 8); ntdll does not.
    static std::string windows_version()
    {
        using RtlGetVersion_t = LONG(WINAPI *)(OSVERSIONINFOW *);
        OSVERSIONINFOW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto fn = ntdll ? reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        if (fn == nullptr || fn(&vi) != 0)
            return "unknown";
        const char *name = (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000) ? "Windows 11"
                         : (vi.dwMajorVersion == 10) ? "Windows 10"
                         : (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 3) ? "Windows 8.1"
                         : (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 2) ? "Windows 8"
                         : (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 1) ? "Windows 7" : "Windows";
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s (build %lu)", name, static_cast<unsigned long>(vi.dwBuildNumber));
        // Wine reports a Windows version too; say so, since it changes what a driver bug can be.
        if (ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr)
            return std::string(buf) + ", under Wine/Proton";
        return buf;
    }

    static std::string gpu_description()
    {
        if (ID3D11Device *dev = D3D11Hook::get().get_device())
        {
            std::string out = "unknown";
            IDXGIDevice *dxgi = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))) && dxgi != nullptr)
            {
                IDXGIAdapter *adapter = nullptr;
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter != nullptr && SUCCEEDED(adapter->GetDesc(&desc)))
                {
                    char vram[48];
                    std::snprintf(vram, sizeof(vram), ", %llu MiB VRAM",
                                  static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)));
                    out = path_utf8(desc.Description) + vram;
                }
                if (adapter != nullptr)
                    adapter->Release();
                dxgi->Release();
            }
            return out;
        }
        if (IDirect3DDevice9 *dev = D3D9Hook::get().get_device())
        {
            std::string out = "unknown";
            IDirect3D9 *d3d = nullptr;
            D3DDEVICE_CREATION_PARAMETERS cp = {};
            D3DADAPTER_IDENTIFIER9 id = {};
            if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d != nullptr && SUCCEEDED(dev->GetCreationParameters(&cp)) &&
                SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id)))
            {
                const LARGE_INTEGER v = id.DriverVersion;
                char drv[64];
                std::snprintf(drv, sizeof(drv), ", driver %u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart),
                              HIWORD(v.LowPart), LOWORD(v.LowPart));
                out = std::string(id.Description) + drv;
            }
            if (d3d != nullptr)
                d3d->Release();
            return out;
        }
        return "no device yet";
    }

    struct InfoRow
    {
        std::string key, value;
        bool mono = false;
    };

    static std::vector<InfoRow> collect_build_info(TextureManager &tm)
    {
        const Configuration &cfg = ConfigManager::get().get_config();
        const auto on = [](bool b) { return b ? "on" : "off"; };

        const std::filesystem::path exe(module_file_name(nullptr));
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&collect_build_info), &self);

        std::vector<InfoRow> rows;
        rows.push_back({ "Texture Toolkit", std::string("v") + TT_VERSION_STRING + (sizeof(void *) == 8 ? " x64" : " x86") +
                                                ", built " __DATE__ " " __TIME__ });
        rows.push_back({ "Game", path_utf8(exe.filename()) });
        rows.push_back({ "Game folder", path_utf8(exe.parent_path()) });
        rows.push_back({ "Loaded from", path_utf8(module_file_name(self)) });
        rows.push_back({ "Windows", windows_version() });
        rows.push_back({ "Graphics API", graphics_api_name() });
        rows.push_back({ "GPU", gpu_description() });
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        char res[32];
        std::snprintf(res, sizeof(res), "%d x %d", static_cast<int>(ds.x), static_cast<int>(ds.y));
        rows.push_back({ "Resolution", res });
        rows.push_back({ "Also hooked in", describe_other_software() });

        const TextureManager::InjectionStats inj = tm.get_injection_stats();
        size_t mods = 0, mods_on = 0;
        for (const TextureManager::ModInfo &m : tm.get_mods())
        {
            if (m.is_base)
                continue;
            ++mods;
            mods_on += m.enabled ? 1 : 0;
        }
        rows.push_back({ "Replacements", std::string("replace ") + on(tm.enable_injection) + ", Special K names " + on(tm.accept_sk_names) +
                                             "; " + std::to_string(inj.files_found) + " found, " + std::to_string(inj.applied) +
                                             " applied, " + std::to_string(inj.failed) + " failed" });
        rows.push_back({ "Mods", std::to_string(mods) + " installed, " + std::to_string(mods_on) + " on" });
        rows.push_back({ "Texture list", std::string("current scene only ") + on(tm.show_current_frame_only) +
                                             ", skip under 16 x 16 " + on(tm.filter_small_textures) +
                                             ", blink selected " + on(tm.highlight_selected) });
        rows.push_back({ "Auto-dump", on(tm.auto_dump) });
        rows.push_back({ "Verbose log", on(cfg.verbose) });
        rows.push_back({ "Panel key", hotkey_name(cfg.hotkey) });
        rows.push_back({ "Resource root", path_utf8(tm.get_resource_root()) });

        const unsigned long long mins = static_cast<unsigned long long>(
            (HookTimings::now() - s_session_start) / HookTimings::ticks_per_ms() / 60000.0);
        char up[48];
        std::snprintf(up, sizeof(up), "%llu min", mins);
        rows.push_back({ "Session", up });
        return rows;
    }

    static void DrawBuildInfoCard()
    {
        TextureManager &tm = TextureManager::get();

        // Module and adapter queries are not free; refresh a couple of times a second at most.
        static std::vector<InfoRow> s_rows;
        static ULONGLONG s_next = 0;
        const ULONGLONG now = GetTickCount64();
        if (s_rows.empty() || now >= s_next)
        {
            s_rows = collect_build_info(tm);
            s_next = now + 2000;
        }

        BeginCard("build", "Build Info",
                  "When reporting a problem, send this along with a verbose log: Copy puts it on the clipboard as text.");
        for (const InfoRow &r : s_rows)
            KeyValue(r.key.c_str(), r.value.c_str(), r.mono);

        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        if (UI::Button("Copy"))
        {
            std::string text = "Texture Toolkit build info\n";
            for (const InfoRow &r : s_rows)
                text += r.key + ": " + r.value + "\n";
            ImGui::SetClipboardText(text.c_str());
            Logger::get().info("[UI] Build info:\n" + text);
            SetStatusMessage("Build info copied to the clipboard, and written to the log.");
        }
        ImGui::SetItemTooltip("Copy every line above as plain text, and write it to the log.");
        EndCard();
    }

    static void DrawDiagnosticsPage()
    {
        PageHeader("Diagnostics", "For tracking down a texture that will not show, or for a bug report.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        const bool d3d9 = D3D9Hook::get().get_device() != nullptr;
        BeginCard("capture", "Find what is on screen",
                  "Writes every texture the game draws with in the next frame to the log, each with its hash, size, format and pool. "
                  "Point the camera at the thing you are looking for, then press this: whatever it is drawn with is in that list, "
                  "and the hash finds it in the Textures page.");
        if (d3d9)
        {
            if (UI::Button("Log this frame", ButtonKind::Primary))
            {
                D3D9Hook::request_frame_capture();
                SetStatusMessage("Wrote every texture drawn this frame to the log.");
            }
        }
        else
        {
            ImGui::BeginDisabled();
            UI::Button("Log this frame", ButtonKind::Primary);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pal().text_faint, "Available in Direct3D 9 games for now.");
        }
        EndCard();

        Configuration &cfg = ConfigManager::get().get_config();
        BeginCard("logging", "Log", "TextureToolkit.log sits next to the .asi. For a bug report, switch on verbose logging, "
                                    "reproduce the problem, and send the log.");
        if (ToggleRow("Verbose logging",
                      "Write a line for every texture and hook call. Takes effect at once; leave it off for normal "
                      "play, since it slows the game.",
                      &cfg.verbose))
        {
            Logger::get().set_min_level(cfg.verbose ? LogLevel::Debug : LogLevel::Info);
            ConfigManager::get().save();
            SetStatusMessage(cfg.verbose ? "Verbose logging on." : "Verbose logging off.");
        }
        EndCard();

        DrawBuildInfoCard();
    }

    // -------------------------------------------------------------------------------------------
    // Window
    // -------------------------------------------------------------------------------------------
    static std::string brand_version_line()
    {
        return std::string("v") + TT_VERSION_STRING + "  \xC2\xB7  " + graphics_api_name();
    }

    // As wide as the sidebar's own contents need, at the sizes they are drawn at (in practice the
    // logo with the name beside it): nothing in it is shrunk or clipped to fit. Sized for the widest the badges get in practice (a five-digit
    // texture count, a three-digit failure count) so the panel does not shift as they change.
    static constexpr float kSidebarPad = 16.0f;
    static float sidebar_width()
    {
        float w = 0.0f;
        float brand = font_strong()->CalcTextSizeA(kSizeBody, FLT_MAX, 0.0f, kBrandTitle).x;
        brand = (std::max)(brand, font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, kBrandCredit).x);
        brand = (std::max)(brand, font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, brand_version_line().c_str()).x);
        w = (std::max)(w, kBrandLogo + kBrandGap + brand + 2.0f);
        w = (std::max)(w, NavItemMinWidth("Textures", "88888"));
        w = (std::max)(w, NavItemMinWidth("Mod files", "888"));
        w = (std::max)(w, NavItemMinWidth("Settings", nullptr));
        w = (std::max)(w, NavItemMinWidth("Diagnostics", nullptr));
        w = (std::max)(w, font_strong()->CalcTextSizeA(kSizeBody, FLT_MAX, 0.0f, kDiscordLabel).x + 28.0f);
        const std::string hint = hotkey_name(ConfigManager::get().get_config().hotkey) + " hides this panel";
        w = (std::max)(w, font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, hint.c_str()).x);
        return std::ceil(w + kSidebarPad * 2.0f);
    }

    static void DrawSidebar(TextureManager &tm, float width, float height)
    {
        Snapshot &snap = snapshot(tm);
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // Brand: the logo, with the name, credit and version beside it, centred on each other.
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const std::string sub = brand_version_line();
            const float h_title = font_strong()->CalcTextSizeA(kSizeBody, FLT_MAX, 0.0f, kBrandTitle).y;
            const float h_small = font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, kBrandCredit).y;
            const float text_h = h_title + 1.0f + h_small * 2.0f;
            const float block_h = (std::max)(kBrandLogo, text_h);

            Logo::draw(dl, ImVec2(p.x, p.y + (block_h - kBrandLogo) * 0.5f), kBrandLogo);
            const float x = p.x + kBrandLogo + kBrandGap;
            float y = p.y + (block_h - text_h) * 0.5f;
            dl->AddText(font_strong(), kSizeBody, ImVec2(x, y), u32(pal().text), kBrandTitle);
            y += h_title + 1.0f;
            dl->AddText(font_body(), kSizeSmall, ImVec2(x, y), u32(pal().text_muted), kBrandCredit);
            y += h_small;
            dl->AddText(font_body(), kSizeSmall, ImVec2(x, y), u32(pal().text_faint), sub.c_str());
            ImGui::Dummy(ImVec2(width, block_h));
        }

        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        SectionLabel("BROWSE");
        ImGui::Dummy(ImVec2(0.0f, 1.0f));

        char badge[24];
        std::snprintf(badge, sizeof(badge), "%zu", snap.textures.size());
        if (NavItem("##nav_tex", Icon::Grid, "Textures", s_page == Page::Textures, badge))
            s_page = Page::Textures;

        // Failed replacement files, as a red count; the page itself says which and why.
        const TextureManager::InjectionStats inj = tm.get_injection_stats();
        char mod_badge[24] = "";
        if (inj.failed > 0)
            std::snprintf(mod_badge, sizeof(mod_badge), "%zu", inj.failed);
        if (NavItem("##nav_mod", Icon::Folder, "Mod files", s_page == Page::ModFiles, mod_badge[0] ? mod_badge : nullptr, &pal().bad))
            s_page = Page::ModFiles;
        if (inj.failed > 0)
            ImGui::SetItemTooltip("%zu replacement file(s) could not be loaded.", inj.failed);

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        SectionLabel("SETUP");
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        if (NavItem("##nav_set", Icon::Gear, "Settings", s_page == Page::Settings))
            s_page = Page::Settings;
        if (NavItem("##nav_diag", Icon::Pulse, "Diagnostics", s_page == Page::Diagnostics))
            s_page = Page::Diagnostics;

        // Footer: the last thing that happened (only once something has), the community link,
        // and how to get out. The status box is sized to its message, since some (a refused dump,
        // say) run to several lines.
        const ImGuiStyle &style = ImGui::GetStyle();
        const float text_w = width - 40.0f;
        const bool has_status = !s_status_message.empty();
        float box_h = 0.0f;
        if (has_status)
        {
            const ImVec2 msg_size = font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, text_w, s_status_message.c_str());
            box_h = (std::max)(40.0f, msg_size.y + 18.0f);
        }
        const float discord_h = ImGui::GetFontSize() + 14.0f; // Button() pads the frame 7px top and bottom
        const float footer_h = (has_status ? box_h + style.ItemSpacing.y : 0.0f) +
                               discord_h + style.ItemSpacing.y + ImGui::GetTextLineHeight();
        const float footer_y = height - footer_h - style.WindowPadding.y;
        if (ImGui::GetCursorPosY() < footer_y)
            ImGui::SetCursorPosY(footer_y);

        if (has_status)
        {
            const ImVec2 f0 = ImGui::GetCursorScreenPos();
            const ImVec2 f1(f0.x + width, f0.y + box_h);
            dl->AddRectFilled(f0, f1, u32(pal().surface_raised), 9.0f);
            dl->AddRect(f0, f1, u32(pal().border), 9.0f, 0, 1.0f);
            draw_icon(dl, Icon::Info, ImVec2(f0.x + 17.0f, f0.y + 17.0f), 13.0f, u32(pal().accent));
            dl->AddText(font_body(), kSizeSmall, ImVec2(f0.x + 31.0f, f0.y + 9.0f), u32(pal().text), s_status_message.c_str(),
                        nullptr, text_w);
            ImGui::Dummy(ImVec2(width, box_h));
        }

        if (UI::Button(kDiscordLabel, ButtonKind::Secondary, ImVec2(width, 0.0f)))
            ShellExecuteW(nullptr, L"open", kDiscordInvite, nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SetItemTooltip("Join Baboon's Workshop");

        char hint[64];
        std::snprintf(hint, sizeof(hint), "%s hides this panel", hotkey_name(ConfigManager::get().get_config().hotkey).c_str());
        ImGui::PushFont(nullptr, kSizeSmall);
        ImGui::TextColored(pal().text_faint, "%s", hint);
        ImGui::PopFont();
    }

    void TextureToolkitUI::draw_ui()
    {
        OSDBanner::get().draw_osd();

        // Only the Textures page aims the in-game blink (it sets it again below); every other
        // page, and a closed panel, leaves the game drawing normally.
        TextureManager::get().set_highlight_target(0);

        if (!is_visible())
        {
            // Drop the pinned preview reference while the panel is hidden.
            TextureManager::get().set_preview_target(0);
            return;
        }

        TextureManager &tm = TextureManager::get();

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x > 0 && display.y > 0)
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(1140, 720), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(920, 580), ImVec2(FLT_MAX, FLT_MAX));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        bool keep_open = true;
        const bool open = ImGui::Begin("Texture Toolkit###TextureToolkit", &keep_open,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (!keep_open)
            set_visible(false);
        if (!open)
        {
            ImGui::End();
            return;
        }

        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const float sidebar_w = sidebar_width();

        // Sidebar ground, darker than the content, joined to the window's rounded left corners.
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(wp, ImVec2(wp.x + sidebar_w, wp.y + ws.y), u32(pal().surface_sunken, 0.85f),
                          ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersLeft);
        dl->AddLine(ImVec2(wp.x + sidebar_w, wp.y), ImVec2(wp.x + sidebar_w, wp.y + ws.y), u32(pal().border));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kSidebarPad, 18.0f));
        ImGui::BeginChild("##sidebar", ImVec2(sidebar_w, ws.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        DrawSidebar(tm, sidebar_w - kSidebarPad * 2.0f, ws.y);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 0.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 20.0f));
        ImGui::BeginChild("##content", ImVec2(0.0f, ws.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        {
            const bool scrolls = (s_page != Page::Textures);
            if (scrolls)
            {
                // Long pages scroll inside their own region; the Textures page manages its own
                // two scrolling panes and fills the height exactly.
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
                ImGui::PushItemWidth(-40.0f);
            }

            switch (s_page)
            {
            case Page::Textures:    DrawTexturesPage(tm); break;
            case Page::ModFiles:    DrawModFilesPage(tm); break;
            case Page::Settings:    DrawSettingsPage(tm); break;
            case Page::Diagnostics: DrawDiagnosticsPage(); break;
            }

            if (scrolls)
            {
                ImGui::PopItemWidth();
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        // Close, top right, in a child window of its own submitted after the content. Drawn inside
        // the content child it sat underneath the scrolling page region, which then took the
        // mouse: ImGui hands hover to the topmost window first, and the last child is topmost.
        {
            const float close_size = 30.0f;
            ImGui::SetCursorPos(ImVec2(ws.x - close_size - 16.0f, 12.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##close_host", ImVec2(close_size, close_size), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            if (IconButton("##close", Icon::Close, "Close the panel", close_size))
                set_visible(false);
            ImGui::EndChild();
        }

        ImGui::End();
    }
}
