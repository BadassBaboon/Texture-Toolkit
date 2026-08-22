#pragma once

#include <atomic>

#include <string>
#include <windows.h>

namespace TextureToolkit
{
    extern std::atomic<bool> g_inside_imgui_render;

    class TextureToolkitUI
    {
    public:
        static void draw_ui();

        // Feeds ImGui the OS mouse position and button state directly, and enables the
        // software cursor. Call once per frame (inside the g_inside_imgui_render window)
        // while the overlay is visible, before ImGui::NewFrame(). Robust against games
        // that grab the mouse via exclusive DirectInput and hide the hardware cursor.
        static void feed_overlay_mouse(HWND hwnd);

        static bool is_visible() { return s_show_ui; }
        static void toggle_visibility() { s_show_ui = !s_show_ui; }
        static void set_visible(bool visible) { s_show_ui = visible; }

    private:
        static bool s_show_ui;
    };
}
