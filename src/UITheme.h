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
    // Descriptions are set in kSizeSmall and have to stay readable over a game at 1440p and up,
    // which 13px Segoe UI was not (15px is); everything else is scaled with them to keep the proportions.
    constexpr float kSizeBody = 17.0f;
    constexpr float kSizeSmall = 15.0f;
    constexpr float kSizeHeading = 18.0f; // card titles, the banner's title, empty-state headings
    constexpr float kSizeTitle = 23.0f;
    constexpr float kSizeStat = 24.0f;
    // Consolas, for hashes and hex flags only. Its letters are wider and taller than Segoe UI's
    // at the same pixel size, so it is set a step down to sit level with kSizeBody text. Paths
    // and other prose stay in Segoe UI: monospace next to it reads as a different size.
    constexpr float kSizeMono = 16.0f;

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
        Grid, Folder, Gear, Pulse, Search, Close, Layers, Info, ChevronUp, ChevronDown,
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
    // `badge_color` defaults to the accent; pass pal().bad for a count of problems.
    bool NavItem(const char *id, Icon icon, const char *label, bool selected, const char *badge = nullptr,
                 const ImVec4 *badge_color = nullptr);

    // Width NavItem needs to show `label` and a badge of `badge` without clipping.
    float NavItemMinWidth(const char *label, const char *badge);

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

    // ---------------------------------------------------------------------------------------
    // Resolution scaling
    //
    // The panel is laid out for a 1440p screen. Rather than multiply every size in it by a scale,
    // ImGui is told the screen is 1440p-sized in its own units and that the framebuffer is denser
    // than that, the way it handles a Retina display. Layout, spacing and the custom drawing all
    // scale together, and text stays sharp, because ImGui rasterises glyphs at the framebuffer
    // density rather than stretching them.
    // ---------------------------------------------------------------------------------------

    // Panel scale for a display `height` pixels tall: UIScale from the ini when it is set,
    // otherwise height / 1440, kept between 0.75 and 3.
    float scale_for_height(float height);

    // The scale in force this frame. 1 until the first frame.
    float current_scale();

    // Call once a frame, after the platform backend's NewFrame (which sets the display size in
    // pixels) and before ImGui::NewFrame. Switches the display to ImGui units and converts the
    // mouse positions waiting in the input queue to match.
    void apply_frame_scale();

    // Turns draw data laid out at a framebuffer scale back into plain pixels, for a renderer that
    // ignores FramebufferScale. The Direct3D 9 backend is one; the Direct3D 11 backend is not.
    void flatten_framebuffer_scale(ImDrawData *draw_data);
}
