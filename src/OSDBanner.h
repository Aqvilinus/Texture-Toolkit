#pragma once

#include <chrono>

namespace TextureToolkit
{
    class OSDBanner
    {
    public:
        static OSDBanner &get();

        void reset();
        void draw_osd();

    private:
        OSDBanner();
        std::chrono::steady_clock::time_point m_start_time;
        bool m_active = true;
    };
}
