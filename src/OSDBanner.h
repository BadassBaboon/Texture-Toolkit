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

        // True while the banner still has something to draw. Lets the present hook skip the whole
        // overlay pass when neither the panel nor the banner is on screen. Accounts for the config
        // toggle as well as the timer: with the banner switched off it never draws, and reporting
        // it as active would keep that per-frame work alive for exactly the people who turned it off.
        bool is_active() const;

    private:
        OSDBanner();
        std::chrono::steady_clock::time_point m_start_time;
        bool m_started = false; // the timer starts on the first drawn frame, not at init
        bool m_active = true;
    };
}
