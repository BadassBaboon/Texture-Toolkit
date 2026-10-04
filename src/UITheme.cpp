#include "UITheme.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace TextureToolkit::UI
{
    namespace
    {
        ImFont *g_body = nullptr;
        ImFont *g_strong = nullptr;
        ImFont *g_mono = nullptr;

        // Right edge of the card currently open, in screen space, or 0 outside a card. A card only
        // indents its left edge, so anything sized to "the rest of the row" stops short here.
        float g_card_right = 0.0f;

        constexpr float kCardPad = 16.0f;
        constexpr float kCardRounding = 10.0f;

        // Taken from the logo: Minecraft-style brick (a55b47 and the lighter b19f99 mortar) set in
        // stone (5d5d5d to 9c9c9c). The surfaces are a warm charcoal under the stone so the panel
        // stays dark; the brick that reads as text is a lightened a55b47, since the logo's own
        // brick is too dark to read on them (3.5:1) and is kept for filled controls instead.
        const Palette kPalette = {
            /* text           */ ImVec4(0.949f, 0.925f, 0.914f, 1.00f),
            /* text_muted     */ ImVec4(0.694f, 0.624f, 0.600f, 1.00f),
            /* text_faint     */ ImVec4(0.533f, 0.533f, 0.533f, 1.00f),
            /* surface        */ ImVec4(0.110f, 0.102f, 0.098f, 0.96f),
            /* surface_raised */ ImVec4(0.165f, 0.149f, 0.141f, 0.88f),
            /* surface_sunken */ ImVec4(0.078f, 0.075f, 0.071f, 0.92f),
            /* border         */ ImVec4(0.612f, 0.612f, 0.612f, 0.13f),
            /* accent         */ ImVec4(0.824f, 0.478f, 0.376f, 1.00f),
            /* accent_fill    */ ImVec4(0.647f, 0.357f, 0.278f, 1.00f),
            /* accent_soft    */ ImVec4(0.722f, 0.400f, 0.310f, 1.00f),
            /* accent_text    */ ImVec4(0.969f, 0.945f, 0.933f, 1.00f),
            /* ok             */ ImVec4(0.498f, 0.812f, 0.580f, 1.00f),
            /* warn           */ ImVec4(0.910f, 0.733f, 0.384f, 1.00f),
            /* bad            */ ImVec4(1.000f, 0.435f, 0.459f, 1.00f),
            /* info           */ ImVec4(0.561f, 0.714f, 0.863f, 1.00f),
            /* neutral        */ ImVec4(0.612f, 0.612f, 0.612f, 1.00f),
        };

        ImVec4 with_alpha(ImVec4 c, float a)
        {
            c.w = a;
            return c;
        }

        float right_edge()
        {
            if (g_card_right > 0.0f)
                return g_card_right;
            return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        }

        std::string windows_font(const char *file)
        {
            char dir[MAX_PATH] = {};
            const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return {};
            std::string path = std::string(dir) + "\\Fonts\\" + file;
            const DWORD attr = GetFileAttributesA(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return {};
            return path;
        }

        ImFont *load_font(const char *primary, const char *secondary, float size)
        {
            ImGuiIO &io = ImGui::GetIO();
            for (const char *file : { primary, secondary })
            {
                if (file == nullptr)
                    continue;
                const std::string path = windows_font(file);
                if (path.empty())
                    continue;

                ImFontConfig cfg;
                cfg.OversampleH = 2;
                cfg.OversampleV = 2;
                if (ImFont *f = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg))
                    return f;
            }
            return nullptr;
        }

        void apply_style()
        {
            ImGuiStyle &s = ImGui::GetStyle();
            const Palette &p = kPalette;

            s.WindowPadding = ImVec2(12.0f, 12.0f);
            s.FramePadding = ImVec2(10.0f, 6.0f);
            s.CellPadding = ImVec2(10.0f, 6.0f);
            s.ItemSpacing = ImVec2(8.0f, 7.0f);
            s.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
            s.IndentSpacing = 18.0f;
            s.ScrollbarSize = 10.0f;
            s.GrabMinSize = 10.0f;

            s.WindowBorderSize = 1.0f;
            s.ChildBorderSize = 1.0f;
            s.PopupBorderSize = 1.0f;
            s.FrameBorderSize = 0.0f;
            s.TabBorderSize = 0.0f;

            s.WindowRounding = 14.0f;
            s.ChildRounding = kCardRounding;
            s.FrameRounding = 7.0f;
            s.PopupRounding = 9.0f;
            s.ScrollbarRounding = 9.0f;
            s.GrabRounding = 7.0f;
            s.TabRounding = 7.0f;

            s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
            s.SeparatorTextBorderSize = 1.0f;
            s.FontSizeBase = kSizeBody;

            ImVec4 *c = s.Colors;
            c[ImGuiCol_Text] = p.text;
            c[ImGuiCol_TextDisabled] = p.text_faint;
            c[ImGuiCol_WindowBg] = p.surface;
            c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_PopupBg] = ImVec4(0.133f, 0.122f, 0.118f, 0.98f);
            c[ImGuiCol_Border] = p.border;
            c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

            c[ImGuiCol_FrameBg] = p.surface_sunken;
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.149f, 0.137f, 0.129f, 1.00f);
            c[ImGuiCol_FrameBgActive] = ImVec4(0.184f, 0.169f, 0.157f, 1.00f);

            c[ImGuiCol_TitleBg] = p.surface;
            c[ImGuiCol_TitleBgActive] = p.surface;
            c[ImGuiCol_TitleBgCollapsed] = p.surface;
            c[ImGuiCol_MenuBarBg] = p.surface;

            c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_ScrollbarGrab] = ImVec4(0.612f, 0.612f, 0.612f, 0.18f);
            c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.612f, 0.612f, 0.612f, 0.30f);
            c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.612f, 0.612f, 0.612f, 0.42f);

            c[ImGuiCol_CheckMark] = p.accent;
            c[ImGuiCol_SliderGrab] = p.accent_fill;
            c[ImGuiCol_SliderGrabActive] = p.accent_soft;

            c[ImGuiCol_Button] = p.surface_raised;
            c[ImGuiCol_ButtonHovered] = ImVec4(0.212f, 0.192f, 0.180f, 1.00f);
            c[ImGuiCol_ButtonActive] = ImVec4(0.255f, 0.231f, 0.216f, 1.00f);

            c[ImGuiCol_Header] = with_alpha(p.accent, 0.20f);
            c[ImGuiCol_HeaderHovered] = with_alpha(p.accent, 0.11f);
            c[ImGuiCol_HeaderActive] = with_alpha(p.accent, 0.26f);

            c[ImGuiCol_Separator] = p.border;
            c[ImGuiCol_SeparatorHovered] = with_alpha(p.accent, 0.50f);
            c[ImGuiCol_SeparatorActive] = p.accent;

            c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_ResizeGripHovered] = with_alpha(p.accent, 0.35f);
            c[ImGuiCol_ResizeGripActive] = with_alpha(p.accent, 0.60f);

            c[ImGuiCol_Tab] = p.surface_raised;
            c[ImGuiCol_TabHovered] = ImVec4(0.212f, 0.192f, 0.180f, 1.00f);
            c[ImGuiCol_TabSelected] = ImVec4(0.275f, 0.247f, 0.231f, 1.00f);

            c[ImGuiCol_TableHeaderBg] = ImVec4(0.133f, 0.122f, 0.118f, 1.00f);
            c[ImGuiCol_TableBorderStrong] = p.border;
            c[ImGuiCol_TableBorderLight] = with_alpha(p.border, 0.06f);
            c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.018f);

            c[ImGuiCol_TextSelectedBg] = with_alpha(p.accent, 0.30f);
            c[ImGuiCol_NavCursor] = p.accent;
            c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
        }
    }

    const Palette &pal() { return kPalette; }

    ImU32 u32(const ImVec4 &c, float alpha_mul)
    {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha_mul * ImGui::GetStyle().Alpha));
    }

    ImFont *font_body() { return g_body; }
    ImFont *font_strong() { return g_strong != nullptr ? g_strong : g_body; }
    ImFont *font_mono() { return g_mono != nullptr ? g_mono : g_body; }

    void init()
    {
        apply_style();

        // The first font added becomes ImGui's default, so the body face goes in first.
        ImGuiIO &io = ImGui::GetIO();
        g_body = load_font("segoeui.ttf", nullptr, kSizeBody);
        if (g_body == nullptr)
            g_body = io.Fonts->AddFontDefault();
        g_strong = load_font("seguisb.ttf", "segoeuib.ttf", kSizeBody);
        g_mono = load_font("consola.ttf", nullptr, kSizeBody);
    }

    // ---------------------------------------------------------------------------------------
    // Icons. Drawn from primitives rather than taken from an icon font: nothing to ship, nothing
    // to load, and no glyph that can come out as '?' on a machine missing the font.
    // ---------------------------------------------------------------------------------------
    void draw_icon(ImDrawList *dl, Icon icon, ImVec2 c, float size, ImU32 col)
    {
        const float h = size * 0.5f;
        const float t = (std::max)(1.3f, size / 11.0f);

        switch (icon)
        {
        case Icon::Grid:
        {
            const float g = size * 0.14f;
            const float cell = (size - g) * 0.5f;
            const float x0 = c.x - h, y0 = c.y - h;
            for (int i = 0; i < 4; ++i)
            {
                const float x = x0 + (i % 2) * (cell + g);
                const float y = y0 + (i / 2) * (cell + g);
                dl->AddRect(ImVec2(x, y), ImVec2(x + cell, y + cell), col, 2.0f, 0, t);
            }
            break;
        }
        case Icon::Folder:
        {
            const ImVec2 pts[] = {
                ImVec2(c.x - h, c.y + h * 0.75f), ImVec2(c.x - h, c.y - h * 0.75f),
                ImVec2(c.x - h * 0.25f, c.y - h * 0.75f), ImVec2(c.x, c.y - h * 0.45f),
                ImVec2(c.x + h, c.y - h * 0.45f), ImVec2(c.x + h, c.y + h * 0.75f),
            };
            dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Gear:
        {
            dl->AddCircle(c, h * 0.62f, col, 0, t);
            dl->AddCircle(c, h * 0.24f, col, 0, t);
            for (int i = 0; i < 8; ++i)
            {
                const float a = i * (3.14159265f / 4.0f);
                const ImVec2 dir(std::cos(a), std::sin(a));
                dl->AddLine(ImVec2(c.x + dir.x * h * 0.62f, c.y + dir.y * h * 0.62f),
                            ImVec2(c.x + dir.x * h * 0.98f, c.y + dir.y * h * 0.98f), col, t * 1.6f);
            }
            break;
        }
        case Icon::Pulse:
        {
            const ImVec2 pts[] = {
                ImVec2(c.x - h, c.y), ImVec2(c.x - h * 0.45f, c.y), ImVec2(c.x - h * 0.2f, c.y - h * 0.75f),
                ImVec2(c.x + h * 0.15f, c.y + h * 0.75f), ImVec2(c.x + h * 0.42f, c.y), ImVec2(c.x + h, c.y),
            };
            dl->AddPolyline(pts, 6, col, ImDrawFlags_None, t);
            break;
        }
        case Icon::Search:
        {
            const ImVec2 lens(c.x - h * 0.18f, c.y - h * 0.18f);
            const float r = h * 0.58f;
            dl->AddCircle(lens, r, col, 0, t);
            dl->AddLine(ImVec2(lens.x + r * 0.72f, lens.y + r * 0.72f), ImVec2(c.x + h * 0.9f, c.y + h * 0.9f), col, t * 1.3f);
            break;
        }
        case Icon::Copy:
        {
            dl->AddRect(ImVec2(c.x - h * 0.85f, c.y - h * 0.45f), ImVec2(c.x + h * 0.35f, c.y + h * 0.9f), col, 2.0f, 0, t);
            dl->AddRect(ImVec2(c.x - h * 0.35f, c.y - h * 0.9f), ImVec2(c.x + h * 0.85f, c.y + h * 0.35f), col, 2.0f, 0, t);
            break;
        }
        case Icon::Download:
        {
            dl->AddLine(ImVec2(c.x, c.y - h * 0.9f), ImVec2(c.x, c.y + h * 0.25f), col, t);
            dl->AddLine(ImVec2(c.x - h * 0.45f, c.y - h * 0.2f), ImVec2(c.x, c.y + h * 0.25f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.45f, c.y - h * 0.2f), ImVec2(c.x, c.y + h * 0.25f), col, t);
            const ImVec2 tray[] = { ImVec2(c.x - h * 0.9f, c.y + h * 0.35f), ImVec2(c.x - h * 0.9f, c.y + h * 0.9f),
                                    ImVec2(c.x + h * 0.9f, c.y + h * 0.9f), ImVec2(c.x + h * 0.9f, c.y + h * 0.35f) };
            dl->AddPolyline(tray, 4, col, ImDrawFlags_None, t);
            break;
        }
        case Icon::Refresh:
        {
            const float r = h * 0.72f;
            dl->PathArcTo(c, r, 3.14159265f * 0.25f, 3.14159265f * 1.85f, 18);
            dl->PathStroke(col, ImDrawFlags_None, t);
            const ImVec2 tip(c.x + std::cos(3.14159265f * 0.25f) * r, c.y + std::sin(3.14159265f * 0.25f) * r);
            dl->AddTriangleFilled(ImVec2(tip.x + h * 0.38f, tip.y - h * 0.05f), ImVec2(tip.x - h * 0.05f, tip.y + h * 0.40f),
                                  ImVec2(tip.x - h * 0.22f, tip.y - h * 0.25f), col);
            break;
        }
        case Icon::Camera:
        {
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.5f), ImVec2(c.x + h, c.y + h * 0.8f), col, 3.0f, 0, t);
            dl->AddLine(ImVec2(c.x - h * 0.35f, c.y - h * 0.5f), ImVec2(c.x - h * 0.2f, c.y - h * 0.85f), col, t);
            dl->AddLine(ImVec2(c.x - h * 0.2f, c.y - h * 0.85f), ImVec2(c.x + h * 0.2f, c.y - h * 0.85f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.2f, c.y - h * 0.85f), ImVec2(c.x + h * 0.35f, c.y - h * 0.5f), col, t);
            dl->AddCircle(ImVec2(c.x, c.y + h * 0.15f), h * 0.38f, col, 0, t);
            break;
        }
        case Icon::Close:
        {
            dl->AddLine(ImVec2(c.x - h * 0.6f, c.y - h * 0.6f), ImVec2(c.x + h * 0.6f, c.y + h * 0.6f), col, t * 1.2f);
            dl->AddLine(ImVec2(c.x + h * 0.6f, c.y - h * 0.6f), ImVec2(c.x - h * 0.6f, c.y + h * 0.6f), col, t * 1.2f);
            break;
        }
        case Icon::Layers:
        {
            const ImVec2 top[] = { ImVec2(c.x, c.y - h * 0.9f), ImVec2(c.x + h, c.y - h * 0.35f),
                                   ImVec2(c.x, c.y + h * 0.2f), ImVec2(c.x - h, c.y - h * 0.35f) };
            dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, t);
            const ImVec2 low[] = { ImVec2(c.x - h, c.y + h * 0.15f), ImVec2(c.x, c.y + h * 0.75f), ImVec2(c.x + h, c.y + h * 0.15f) };
            dl->AddPolyline(low, 3, col, ImDrawFlags_None, t);
            break;
        }
        case Icon::Info:
        {
            dl->AddCircle(c, h * 0.95f, col, 0, t);
            dl->AddLine(ImVec2(c.x, c.y - h * 0.05f), ImVec2(c.x, c.y + h * 0.5f), col, t * 1.2f);
            dl->AddCircleFilled(ImVec2(c.x, c.y - h * 0.42f), t * 0.9f, col);
            break;
        }
        case Icon::ChevronUp:
        case Icon::ChevronDown:
        {
            const float dy = (icon == Icon::ChevronUp) ? -1.0f : 1.0f;
            const ImVec2 pts[] = { ImVec2(c.x - h * 0.6f, c.y - dy * h * 0.3f), ImVec2(c.x, c.y + dy * h * 0.3f),
                                   ImVec2(c.x + h * 0.6f, c.y - dy * h * 0.3f) };
            dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t * 1.2f);
            break;
        }
        }
    }

    // ---------------------------------------------------------------------------------------
    // Cards
    // ---------------------------------------------------------------------------------------
    void BeginCard(const char *id, const char *title, const char *subtitle)
    {
        ImGui::PushID(id);

        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGuiStorage *storage = ImGui::GetStateStorage();
        const float prev_h = storage->GetFloat(ImGui::GetID("##card_h"), 0.0f);

        if (prev_h > 0.0f)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 end(start.x + width, start.y + prev_h);
            dl->AddRectFilled(start, end, u32(kPalette.surface_raised), kCardRounding);
            dl->AddRect(start, end, u32(kPalette.border), kCardRounding, 0, 1.0f);
        }

        storage->SetFloat(ImGui::GetID("##card_y"), start.y);
        g_card_right = start.x + width - kCardPad;

        ImGui::Dummy(ImVec2(0.0f, kCardPad - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Indent(kCardPad);
        ImGui::PushTextWrapPos(g_card_right - ImGui::GetWindowPos().x);

        if (title != nullptr)
        {
            ImGui::PushFont(font_strong(), kSizeBody + 1.0f);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
        }
        if (subtitle != nullptr)
        {
            ImGui::PushFont(nullptr, kSizeSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, kPalette.text_muted);
            ImGui::TextWrapped("%s", subtitle);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        if (title != nullptr || subtitle != nullptr)
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    void EndCard()
    {
        ImGui::Dummy(ImVec2(0.0f, kCardPad - ImGui::GetStyle().ItemSpacing.y));
        ImGui::PopTextWrapPos();
        ImGui::Unindent(kCardPad);

        ImGuiStorage *storage = ImGui::GetStateStorage();
        const float top = storage->GetFloat(ImGui::GetID("##card_y"), 0.0f);
        const float height = ImGui::GetCursorScreenPos().y - top - ImGui::GetStyle().ItemSpacing.y;
        if (height > 0.0f)
            storage->SetFloat(ImGui::GetID("##card_h"), height);

        g_card_right = 0.0f;
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    // ---------------------------------------------------------------------------------------
    // Controls
    // ---------------------------------------------------------------------------------------
    ImVec2 ToggleSwitchSize()
    {
        const float height = ImGui::GetFrameHeight() * 0.74f;
        return ImVec2(height * 1.8f, height);
    }

    float CardRightEdge()
    {
        return right_edge();
    }

    bool ToggleSwitch(const char *id, bool *v)
    {
        const float height = ImGui::GetFrameHeight() * 0.74f;
        const float width = height * 1.8f;
        const float radius = height * 0.5f;
        const ImVec2 p = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton(id, ImVec2(width, height));
        const bool clicked = ImGui::IsItemClicked();
        if (clicked)
            *v = !*v;
        const bool hovered = ImGui::IsItemHovered();

        // Slide the knob instead of jumping it. The position is kept per switch.
        ImGuiStorage *storage = ImGui::GetStateStorage();
        const ImGuiID anim_id = ImGui::GetItemID();
        float t = storage->GetFloat(anim_id, *v ? 1.0f : 0.0f);
        const float target = *v ? 1.0f : 0.0f;
        const float step = ImGui::GetIO().DeltaTime * 10.0f;
        t = (t < target) ? (std::min)(target, t + step) : (std::max)(target, t - step);
        storage->SetFloat(anim_id, t);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec4 off = hovered ? ImVec4(0.255f, 0.231f, 0.216f, 1.00f) : ImVec4(0.212f, 0.192f, 0.180f, 1.00f);
        const ImVec4 on = hovered ? kPalette.accent_soft : kPalette.accent_fill;
        const ImVec4 track(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t,
                           off.z + (on.z - off.z) * t, 1.0f);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), u32(track), radius);

        const float knob_x = p.x + radius + t * (width - 2.0f * radius);
        const ImVec4 knob = (t > 0.5f) ? kPalette.accent_text : ImVec4(0.784f, 0.784f, 0.784f, 1.00f);
        dl->AddCircleFilled(ImVec2(knob_x, p.y + radius), radius - 3.0f, u32(knob));
        return clicked;
    }

    bool ToggleRow(const char *label, const char *subtext, bool *v, const char *tooltip)
    {
        ImGui::PushID(label);

        const float switch_h = ImGui::GetFrameHeight() * 0.74f;
        const float switch_w = switch_h * 1.8f;
        const float right = right_edge();
        const ImVec2 start = ImGui::GetCursorScreenPos();

        ImGui::PushTextWrapPos(right - switch_w - 18.0f - ImGui::GetWindowPos().x);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(label);
        if (subtext != nullptr && subtext[0] != '\0')
        {
            ImGui::PushFont(nullptr, kSizeSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, kPalette.text_muted);
            ImGui::TextWrapped("%s", subtext);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        ImGui::PopTextWrapPos();

        // The words are part of the control: clicking the label flips the switch too.
        bool changed = false;
        if (ImGui::IsItemClicked())
        {
            *v = !*v;
            changed = true;
        }
        if (tooltip != nullptr)
            ImGui::SetItemTooltip("%s", tooltip);

        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(right - switch_w, start.y + (ImGui::GetTextLineHeight() - switch_h) * 0.5f + 1.0f));
        changed |= ToggleSwitch("##switch", v);
        if (tooltip != nullptr)
            ImGui::SetItemTooltip("%s", tooltip);

        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::PopID();
        return changed;
    }

    bool Button(const char *label, ButtonKind kind, ImVec2 size)
    {
        int colors = 0;
        switch (kind)
        {
        case ButtonKind::Primary:
            ImGui::PushStyleColor(ImGuiCol_Button, kPalette.accent_fill);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPalette.accent_soft);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.557f, 0.302f, 0.235f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, kPalette.accent_text);
            colors = 4;
            break;
        case ButtonKind::Secondary:
            ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(kPalette.accent, 0.14f));
            colors = 1;
            break;
        case ButtonKind::Ghost:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, kPalette.text_muted);
            colors = 2;
            break;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 7.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, kind == ButtonKind::Secondary ? 1.0f : 0.0f);
        ImGui::PushFont(font_strong(), 0.0f);
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopFont();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(colors);
        return pressed;
    }

    bool IconButton(const char *id, Icon icon, const char *tooltip, float size)
    {
        if (size <= 0.0f)
            size = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (hovered)
            dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), u32(ImVec4(1, 1, 1, 0.06f)), 7.0f);
        draw_icon(dl, icon, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.46f,
                  u32(hovered ? kPalette.text : kPalette.text_muted));
        if (tooltip != nullptr)
            ImGui::SetItemTooltip("%s", tooltip);
        return pressed;
    }

    bool NavItem(const char *id, Icon icon, const char *label, bool selected, const char *badge)
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight() + 10.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();

        const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        if (selected)
            dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), u32(kPalette.accent_fill), 9.0f);
        else if (hovered)
            dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), u32(ImVec4(1, 1, 1, 0.05f)), 9.0f);

        const ImU32 fg = u32(selected ? kPalette.accent_text : (hovered ? kPalette.text : kPalette.text_muted));
        const float cy = p.y + height * 0.5f;
        draw_icon(dl, icon, ImVec2(p.x + 20.0f, cy), 15.0f, fg);

        ImFont *font = selected ? font_strong() : font_body();
        const float font_size = ImGui::GetFontSize();
        dl->AddText(font, font_size, ImVec2(p.x + 38.0f, cy - font_size * 0.5f), fg, label);

        if (badge != nullptr && badge[0] != '\0')
        {
            const ImVec2 ts = font_body()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, badge);
            const ImVec2 b0(p.x + width - ts.x - 22.0f, cy - ts.y * 0.5f - 2.0f);
            const ImVec2 b1(p.x + width - 10.0f, cy + ts.y * 0.5f + 2.0f);
            dl->AddRectFilled(b0, b1, u32(selected ? with_alpha(kPalette.accent_text, 0.14f) : with_alpha(kPalette.accent, 0.16f)), 999.0f);
            dl->AddText(font_body(), kSizeSmall, ImVec2(b0.x + 6.0f, b0.y + 2.0f),
                        u32(selected ? kPalette.accent_text : kPalette.accent), badge);
        }
        else
        {
            dl->AddCircleFilled(ImVec2(p.x + width - 16.0f, cy), 2.6f,
                                u32(selected ? kPalette.accent_text : kPalette.text_faint, selected ? 1.0f : 0.6f));
        }
        return pressed;
    }

    ImVec2 PillSize(const char *text)
    {
        const ImVec2 ts = font_strong()->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, text);
        return ImVec2(ts.x + 16.0f, ts.y + 4.0f);
    }

    void Pill(const char *text, const ImVec4 &color)
    {
        const ImVec2 size = PillSize(text);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), u32(with_alpha(color, 0.15f)), 999.0f);
        dl->AddText(font_strong(), kSizeSmall, ImVec2(p.x + 8.0f, p.y + 2.0f), u32(color), text);
    }

    void StatTile(const char *caption, const char *value, const ImVec4 &accent, float width, const char *tooltip)
    {
        const float height = 62.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, height));
        if (tooltip != nullptr)
            ImGui::SetItemTooltip("%s", tooltip);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), u32(kPalette.surface_raised), kCardRounding);
        dl->AddRect(p, ImVec2(p.x + width, p.y + height), u32(kPalette.border), kCardRounding, 0, 1.0f);
        dl->AddRectFilled(ImVec2(p.x + 12.0f, p.y + 15.0f), ImVec2(p.x + 15.0f, p.y + height - 15.0f), u32(accent), 2.0f);

        dl->AddText(font_strong(), kSizeStat, ImVec2(p.x + 25.0f, p.y + 9.0f), u32(kPalette.text), value);
        dl->AddText(font_body(), kSizeSmall, ImVec2(p.x + 25.0f, p.y + 9.0f + kSizeStat + 3.0f), u32(kPalette.text_muted), caption);
    }

    void SectionLabel(const char *text)
    {
        ImGui::PushFont(font_strong(), kSizeSmall - 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, kPalette.text_faint);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    void KeyValue(const char *key, const char *value, bool mono, const ImVec4 *value_color)
    {
        constexpr float kKeyWidth = 104.0f;
        const float x = ImGui::GetCursorPosX();

        ImGui::PushStyleColor(ImGuiCol_Text, kPalette.text_muted);
        ImGui::TextUnformatted(key);
        ImGui::PopStyleColor();

        ImGui::SameLine(x + kKeyWidth);
        if (mono)
        {
            // Consolas runs larger than Segoe UI at the same size, so it is set smaller, and
            // nudged down to share the label's baseline instead of its top.
            const float body_h = ImGui::GetTextLineHeight();
            ImGui::PushFont(font_mono(), kSizeSmall + 0.5f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (body_h - ImGui::GetTextLineHeight()) * 0.5f + 1.0f);
        }
        if (value_color != nullptr)
            ImGui::PushStyleColor(ImGuiCol_Text, *value_color);
        ImGui::TextWrapped("%s", value);
        if (value_color != nullptr)
            ImGui::PopStyleColor();
        if (mono)
            ImGui::PopFont();
    }

    void Checkerboard(ImDrawList *dl, ImVec2 a, ImVec2 b, float cell, float rounding)
    {
        dl->AddRectFilled(a, b, u32(ImVec4(0.200f, 0.200f, 0.200f, 1.00f)), rounding);
        dl->PushClipRect(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), true);
        const ImU32 light = u32(ImVec4(0.22f, 0.245f, 0.32f, 1.0f));
        int row = 0;
        for (float y = a.y; y < b.y; y += cell, ++row)
        {
            for (float x = a.x + ((row & 1) ? cell : 0.0f); x < b.x; x += cell * 2.0f)
                dl->AddRectFilled(ImVec2(x, y), ImVec2((std::min)(x + cell, b.x), (std::min)(y + cell, b.y)), light);
        }
        dl->PopClipRect();
    }
}
