#pragma once

#include <imgui.h>

// Visual language for the overlay: one palette, one set of fonts, and the handful of widgets the
// panel is built from. Kept apart from the panel's logic so the two can change independently, and
// so the look is defined in exactly one place.
namespace TextureToolkit::UI
{
    // Applies the style and loads the fonts. Call once, right after ImGui::CreateContext().
    void init();

    // Fonts. Each falls back to ImGui's built-in font when its file is not on the system, so a
    // machine without Segoe UI or Consolas gets a plainer panel rather than no panel.
    ImFont *font_body();
    ImFont *font_strong();
    ImFont *font_mono();

    // Sizes, in unscaled pixels, for PushFont(font, size).
    constexpr float kSizeBody = 15.0f;
    constexpr float kSizeSmall = 13.0f;
    constexpr float kSizeTitle = 21.0f;
    constexpr float kSizeStat = 22.0f;

    struct Palette
    {
        ImVec4 text, text_muted, text_faint;
        ImVec4 surface, surface_raised, surface_sunken, border;
        // accent: brick, light enough to read as text or an icon on the dark surfaces.
        // accent_fill: the logo's own brick, for filled controls; accent_soft is its hover.
        // accent_text: text and icons drawn ON a filled control.
        ImVec4 accent, accent_fill, accent_soft, accent_text;
        ImVec4 ok, warn, bad, info, neutral;
    };
    const Palette &pal();

    ImU32 u32(const ImVec4 &c, float alpha_mul = 1.0f);

    enum class Icon
    {
        Grid, Folder, Gear, Pulse, Search, Copy, Download, Refresh, Camera, Close, Layers, Info,
        ChevronUp, ChevronDown,
    };
    void draw_icon(ImDrawList *dl, Icon icon, ImVec2 center, float size, ImU32 col);

    // Section card. Painted into the window's draw list behind its contents using the height it
    // had last frame, instead of being a child window: nested auto-sizing children inside a
    // scrolling parent fight the clipper and snap the scroll position back to the top.
    void BeginCard(const char *id, const char *title = nullptr, const char *subtitle = nullptr);
    void EndCard();

    bool ToggleSwitch(const char *id, bool *v);
    ImVec2 ToggleSwitchSize();

    // Screen-space right edge of the current card's content (or of the window's when outside one).
    float CardRightEdge();

    // A labelled switch spanning the row, with an optional second line explaining it.
    bool ToggleRow(const char *label, const char *subtext, bool *v, const char *tooltip = nullptr);

    enum class ButtonKind { Primary, Secondary, Ghost };
    bool Button(const char *label, ButtonKind kind = ButtonKind::Secondary, ImVec2 size = ImVec2(0, 0));
    bool IconButton(const char *id, Icon icon, const char *tooltip, float size = 0.0f);

    // Sidebar entry. Returns true when clicked.
    bool NavItem(const char *id, Icon icon, const char *label, bool selected, const char *badge = nullptr);

    // Small rounded label in a tinted colour, for statuses.
    void Pill(const char *text, const ImVec4 &color);
    ImVec2 PillSize(const char *text);

    // A compact figure with a caption under it and a coloured edge on the left.
    void StatTile(const char *caption, const char *value, const ImVec4 &accent, float width, const char *tooltip = nullptr);

    // Muted small caps label that heads a group of controls.
    void SectionLabel(const char *text);

    // Key/value row for the inspector grids. Values in mono when they are identifiers.
    void KeyValue(const char *key, const char *value, bool mono = false, const ImVec4 *value_color = nullptr);

    // A checkerboard, so transparent pixels in a preview read as transparent rather than black.
    void Checkerboard(ImDrawList *dl, ImVec2 a, ImVec2 b, float cell, float rounding);
}
