#include "OSDBanner.h"
#include "Version.h"
#include "Config.h"
#include "TextureManager.h"
#include "UITheme.h"
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <string>

namespace TextureToolkit
{
    OSDBanner &OSDBanner::get()
    {
        static OSDBanner instance;
        return instance;
    }

    OSDBanner::OSDBanner()
    {
        reset();
    }

    void OSDBanner::reset()
    {
        m_start_time = std::chrono::steady_clock::now();
        m_started = false; // (re)armed; the clock starts on the first drawn frame
        m_active = true;
    }

    bool OSDBanner::is_active() const
    {
        return m_active && ConfigManager::get().get_config().show_osd_banner;
    }

    void OSDBanner::draw_osd()
    {
        const auto &config = ConfigManager::get().get_config();
        if (!config.show_osd_banner || !m_active)
            return;

        // Start the countdown on the first frame we actually draw, not at DLL init. A game can
        // spend a long time loading before it presents (Deus Ex: Mankind Divided takes ~21s), and
        // timing from init meant the banner had already expired before the overlay existed.
        if (!m_started)
        {
            m_started = true;
            m_start_time = std::chrono::steady_clock::now();
        }

        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - m_start_time).count();

        float duration = config.osd_duration_seconds;
        if (elapsed >= duration)
        {
            m_active = false;
            return;
        }

        float alpha = 1.0f;
        if (elapsed < 0.5f)
        {
            alpha = elapsed / 0.5f; // Fade in
        }
        else if (elapsed > duration - 1.5f)
        {
            alpha = (duration - elapsed) / 1.5f; // Fade out
        }
        alpha = (std::max)(0.0f, (std::min)(1.0f, alpha));

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x <= 0.0f || display.y <= 0.0f)
            return;

        // A toast, drawn straight onto the foreground list rather than as a window: it can never
        // take focus or input from the game, and it sits above everything including the panel.
        using namespace UI;
        const Palette &p = pal();
        ImDrawList *dl = ImGui::GetForegroundDrawList();

        const std::string key = hotkey_name(config.hotkey);
        const char *title = "Texture Toolkit";
        const char *version = "v" TT_VERSION_STRING;
        char state[96];
        std::snprintf(state, sizeof(state), "Replacements %s  \xC2\xB7  Auto-dump %s",
                      TextureManager::get().enable_injection ? "on" : "off",
                      TextureManager::get().auto_dump ? "on" : "off");

        ImFont *strong = font_strong();
        ImFont *body = font_body();
        const ImVec2 title_sz = strong->CalcTextSizeA(kSizeBody + 1.0f, FLT_MAX, 0.0f, title);
        const ImVec2 ver_sz = body->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, version);
        const ImVec2 press_sz = body->CalcTextSizeA(kSizeBody, FLT_MAX, 0.0f, "Press");
        const ImVec2 key_sz = strong->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, key.c_str());
        const ImVec2 open_sz = body->CalcTextSizeA(kSizeBody, FLT_MAX, 0.0f, "to open the panel");
        const ImVec2 state_sz = body->CalcTextSizeA(kSizeSmall, FLT_MAX, 0.0f, state);

        const float icon = 40.0f, pad = 16.0f, gap = 14.0f;
        const float line2_w = press_sz.x + 6.0f + (key_sz.x + 16.0f) + 6.0f + open_sz.x;
        const float text_w = (std::max)({ title_sz.x + 8.0f + ver_sz.x, line2_w, state_sz.x });
        const float w = pad + icon + gap + text_w + pad + 4.0f;
        const float h = 84.0f;

        // Slides up a little as it fades in.
        const float rise = (1.0f - alpha) * 10.0f;
        const ImVec2 a(display.x * 0.5f - w * 0.5f, display.y - 72.0f - h + rise);
        const ImVec2 b(a.x + w, a.y + h);

        dl->AddRectFilled(ImVec2(a.x, a.y + 6.0f), ImVec2(b.x, b.y + 6.0f), u32(ImVec4(0, 0, 0, 0.35f * alpha)), 16.0f);
        dl->AddRectFilled(a, b, u32(ImVec4(p.surface.x, p.surface.y, p.surface.z, 0.95f * alpha)), 16.0f);
        dl->AddRect(a, b, u32(ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f * alpha)), 16.0f, 0, 1.0f);

        const ImVec2 i0(a.x + pad, a.y + (h - icon) * 0.5f);
        dl->AddRectFilled(i0, ImVec2(i0.x + icon, i0.y + icon), u32(ImVec4(p.accent.x, p.accent.y, p.accent.z, alpha)), 10.0f);
        draw_icon(dl, Icon::Layers, ImVec2(i0.x + icon * 0.5f, i0.y + icon * 0.5f), 20.0f,
                  u32(ImVec4(p.accent_text.x, p.accent_text.y, p.accent_text.z, alpha)));

        const auto fade = [alpha](ImVec4 c) { c.w *= alpha; return c; };
        float x = i0.x + icon + gap;
        float y = a.y + 13.0f;
        dl->AddText(strong, kSizeBody + 1.0f, ImVec2(x, y), u32(fade(p.text)), title);
        dl->AddText(body, kSizeSmall, ImVec2(x + title_sz.x + 8.0f, y + 2.0f), u32(fade(p.text_muted)), version);

        y += title_sz.y + 4.0f;
        dl->AddText(body, kSizeBody, ImVec2(x, y), u32(fade(p.text_muted)), "Press");
        const ImVec2 k0(x + press_sz.x + 6.0f, y - 1.0f);
        const ImVec2 k1(k0.x + key_sz.x + 16.0f, k0.y + press_sz.y + 2.0f);
        dl->AddRectFilled(k0, k1, u32(fade(p.surface_raised)), 6.0f);
        dl->AddRect(k0, k1, u32(fade(ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.45f))), 6.0f, 0, 1.0f);
        dl->AddText(strong, kSizeSmall, ImVec2(k0.x + 8.0f, k0.y + (press_sz.y + 2.0f - key_sz.y) * 0.5f), u32(fade(p.accent)), key.c_str());
        dl->AddText(body, kSizeBody, ImVec2(k1.x + 6.0f, y), u32(fade(p.text_muted)), "to open the panel");

        y += press_sz.y + 5.0f;
        dl->AddText(body, kSizeSmall, ImVec2(x, y), u32(fade(p.text_faint)), state);
    }
}
