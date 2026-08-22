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

        bool is_active() const { return m_active; }

    private:
        OSDBanner();
        std::chrono::steady_clock::time_point m_start_time;
        bool m_started = false; // the timer starts on the first drawn frame, not at init
        bool m_active = true;
    };
}
