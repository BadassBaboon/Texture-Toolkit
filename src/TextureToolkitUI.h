#pragma once

#include <atomic>

#include <string>
#include <windows.h>

namespace TextureToolkit
{
    // Lets our own code read real key and mouse state through the input hooks, which mask it from
    // the game while the panel is open. Per thread: one global flag, raised while the panel was
    // built, let any other game thread polling keys in that window read straight through.
    extern thread_local bool g_inside_imgui_render;

    class TextureToolkitUI
    {
    public:
        static void draw_ui();

        // Feeds ImGui the OS mouse position and button state directly, and enables the
        // software cursor. Call once per frame (inside the g_inside_imgui_render window)
        // while the overlay is visible, before ImGui::NewFrame(). Robust against games
        // that grab the mouse via exclusive DirectInput and hide the hardware cursor.
        static void feed_overlay_mouse(HWND hwnd);

        // Puts the OS cursor's display count back to what it was when the panel opened. Call
        // every frame while the panel is closed; it does nothing unless feed_overlay_mouse
        // changed the count.
        static void release_overlay_mouse();

        // Atomic: toggled on the render thread, read by every thread the input hooks run on.
        static bool is_visible() { return s_show_ui.load(std::memory_order_relaxed); }
        static void toggle_visibility() { s_show_ui.store(!is_visible(), std::memory_order_relaxed); }
        static void set_visible(bool visible) { s_show_ui.store(visible, std::memory_order_relaxed); }

    private:
        static std::atomic<bool> s_show_ui;
    };
}
