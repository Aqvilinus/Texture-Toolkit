#pragma once

#include <atomic>

#include <string>
#include <windows.h>

namespace TextureToolkit
{
    class TextureToolkitUI
    {
    public:
        static void draw_ui();

        // Feeds ImGui the OS mouse position and button state directly, and enables the
        // software cursor. Call once per frame while the overlay is visible, before
        // ImGui::NewFrame(). Robust against games that grab the mouse via exclusive
        // DirectInput and hide the hardware cursor.
        static void feed_overlay_mouse(HWND hwnd);

        // Toggled from whichever thread polls the hotkey, read by the one drawing the frame.
        static bool is_visible() { return s_show_ui.load(std::memory_order_relaxed); }
        static void toggle_visibility() { s_show_ui.store(!is_visible(), std::memory_order_relaxed); }
        static void set_visible(bool visible) { s_show_ui.store(visible, std::memory_order_relaxed); }

    private:
        static std::atomic<bool> s_show_ui;
    };
}
