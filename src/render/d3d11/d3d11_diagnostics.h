#pragma once

// Frame timings, kept out of the shipping build entirely. Averages hide stutter, so the reporting
// here is per-slow-frame; the two QueryPerformanceCounter calls it needs on a hot path cost about
// 0.1 ms/frame, which is why none of it is compiled unless asked for.
#include "core/config.h"
#include "core/logger.h"
#include "texture/texture_manager.h"
#include "render/d3d11/d3d11_textures.h"

#include <atomic>
#include <windows.h>

#if TT_DIAGNOSTICS
namespace TextureToolkit
{
    struct QpcTimer
    {
        LARGE_INTEGER start;
        std::atomic<uint64_t> &sink;
        explicit QpcTimer(std::atomic<uint64_t> &s) : sink(s) { QueryPerformanceCounter(&start); }
        ~QpcTimer()
        {
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            sink.fetch_add(static_cast<uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
        }
    };

    inline std::atomic<uint64_t> s_stat_map_records{0};

    // Raw QPC ticks, converted at report time. s_t_present is the game's OWN Present: time
    // there means the cost is in what we did to the driver, not in our detours.
    inline std::atomic<uint64_t> s_t_present{0};
    inline std::atomic<uint64_t> s_t_overlay{0};
    inline std::atomic<uint64_t> s_t_mapunmap{0};

    // Averages are useless for stutter: one 30 ms hitch among 400 frames moves a three-second
    // average by 0.07 ms, while being exactly what the player sees.
    inline constexpr double kSlowFrameMs = 20.0;

    // Two QueryPerformanceCounter calls on a path walked thousands of times per frame cost
    // ~0.1 ms/frame -- most of what that hook then reports as its own cost.
    #define TT_PROFILE_BIND_HOOK 0

    inline void report_slow_frame(double frame_ms, double overlay_ms,
                                  double hash_ms, double hash_mb, uint64_t builds)
    {
        static int s_reported = 0;
        if (s_reported >= 60) // enough to characterise a session without flooding the log
            return;
        ++s_reported;

        Logger::get().info(Logger::fmt(
            "[D3D11Hook] [slow frame] %.1f ms | ours: overlay %.2f, hash %.2f (%.1f MB) | builds: %llu",
            frame_ms, overlay_ms, hash_ms, hash_mb, static_cast<unsigned long long>(builds)));
    }

    inline double ticks_ms(uint64_t ticks, const LARGE_INTEGER &freq, uint64_t frames)
    {
        if (freq.QuadPart == 0 || frames == 0)
            return 0.0;
        return (static_cast<double>(ticks) * 1000.0) / (static_cast<double>(freq.QuadPart) * static_cast<double>(frames));
    }

    // The interval is wall-clock rather than a frame count because the scenes worth measuring
    // are the slow ones: 300 frames at 12 fps is 25 seconds, and a short session ends first.
    inline void report_perf()
    {
        static LARGE_INTEGER s_freq = {};
        static LARGE_INTEGER s_last = {};
        static uint64_t s_frames = 0;

        if (s_freq.QuadPart == 0)
        {
            QueryPerformanceFrequency(&s_freq);
            QueryPerformanceCounter(&s_last);
        }

        ++s_frames;

        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        const double secs = static_cast<double>(now.QuadPart - s_last.QuadPart) / static_cast<double>(s_freq.QuadPart);
        if (secs < 3.0)
            return;
        s_last = now;

        D3D11TextureManager &tm = D3D11TextureManager::get();
        const uint64_t maps = s_stat_map_records.exchange(0, std::memory_order_relaxed);

        // Every field here must have an argument behind it: the counters this line used to print
        // were removed and their placeholders left, so it spent a while reading past the end of
        // the argument list and printing whatever was there.
        Logger::get().info(Logger::fmt(
            "[D3D11Hook] [perf] %.1f fps (%llu frames / %.1fs) | per frame: %llu maps, %llu builds"
            " | ms/frame: present %.3f, overlay %.3f, map %.3f, hash %.3f (%.0f MB this window)",
            secs > 0.0 ? static_cast<double>(s_frames) / secs : 0.0,
            static_cast<unsigned long long>(s_frames), secs,
            static_cast<unsigned long long>(maps / s_frames),
            static_cast<unsigned long long>(tm.stat_builds.exchange(0, std::memory_order_relaxed)),
            ticks_ms(s_t_present.exchange(0, std::memory_order_relaxed), s_freq, s_frames),
            ticks_ms(s_t_overlay.exchange(0, std::memory_order_relaxed), s_freq, s_frames),
            ticks_ms(s_t_mapunmap.exchange(0, std::memory_order_relaxed), s_freq, s_frames),
            ticks_ms(tm.stat_hash_ticks.exchange(0, std::memory_order_relaxed), s_freq, s_frames),
            static_cast<double>(tm.stat_hash_bytes.exchange(0, std::memory_order_relaxed)) / (1024.0 * 1024.0)));

        s_frames = 0;
    }

}
#endif
