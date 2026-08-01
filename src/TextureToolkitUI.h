#pragma once

#include <string>

namespace TextureToolkit
{
    class TextureToolkitUI
    {
    public:
        static void draw_ui(void *runtime = nullptr);

        static bool is_visible() { return s_show_ui; }
        static void toggle_visibility() { s_show_ui = !s_show_ui; }
        static void set_visible(bool visible) { s_show_ui = visible; }

    private:
        static bool s_show_ui;
    };
}
