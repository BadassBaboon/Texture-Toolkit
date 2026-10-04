#include "HookTimings.h"
#include "Logger.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace TextureToolkit::HookTimings
{
    namespace
    {
        constexpr int kSites = static_cast<int>(Site::Count);
        constexpr double kReportSeconds = 5.0;
        constexpr double kSlowFrameMs = 20.0;

        const char *const kNames[kSites] = {
            "D3D9 upload", "D3D9 bind", "D3D11 create", "D3D11 unmap", "D3D11 bind", "overlay",
        };

        // Written from whatever thread the game calls a hook on, so every field is atomic.
        struct Counter
        {
            std::atomic<uint64_t> calls{0};
            std::atomic<uint64_t> total{0};
            std::atomic<uint64_t> worst{0};
        };
        Counter g_counters[kSites];

        double ticks_per_ms()
        {
            static const double value = []()
            {
                LARGE_INTEGER f = {};
                QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart) / 1000.0;
            }();
            return value;
        }

        // Frame tracking runs on the render thread only.
        uint64_t g_last_frame = 0;
        uint64_t g_window_start = 0;
        uint64_t g_frames = 0;
        uint64_t g_slow_frames = 0;
        uint64_t g_worst_frame = 0;
    }

    bool enabled()
    {
        return Logger::get().debug_enabled();
    }

    uint64_t now()
    {
        LARGE_INTEGER t = {};
        QueryPerformanceCounter(&t);
        return static_cast<uint64_t>(t.QuadPart);
    }

    void record(Site site, uint64_t start)
    {
        const uint64_t elapsed = now() - start;
        Counter &c = g_counters[static_cast<int>(site)];
        c.calls.fetch_add(1, std::memory_order_relaxed);
        c.total.fetch_add(elapsed, std::memory_order_relaxed);
        uint64_t prev = c.worst.load(std::memory_order_relaxed);
        while (elapsed > prev && !c.worst.compare_exchange_weak(prev, elapsed, std::memory_order_relaxed))
        {
        }
    }

    void frame()
    {
        if (!enabled())
        {
            // Start clean when verbose logging is switched back on: a window cut short by
            // switching it off must not have its figures folded into the next report.
            if (g_window_start != 0)
            {
                for (Counter &c : g_counters)
                {
                    c.calls.store(0, std::memory_order_relaxed);
                    c.total.store(0, std::memory_order_relaxed);
                    c.worst.store(0, std::memory_order_relaxed);
                }
                g_frames = g_slow_frames = g_worst_frame = 0;
            }
            g_last_frame = 0;
            g_window_start = 0;
            return;
        }

        const uint64_t t = now();
        const double per_ms = ticks_per_ms();
        if (g_last_frame != 0)
        {
            const uint64_t dt = t - g_last_frame;
            ++g_frames;
            if (dt / per_ms > kSlowFrameMs)
                ++g_slow_frames;
            if (dt > g_worst_frame)
                g_worst_frame = dt;
        }
        g_last_frame = t;

        if (g_window_start == 0)
        {
            g_window_start = t;
            return;
        }
        const double window_ms = (t - g_window_start) / per_ms;
        if (window_ms < kReportSeconds * 1000.0)
            return;

        char line[160];
        std::snprintf(line, sizeof(line), "[Timing] %.1fs: %llu frames, %llu over %.0f ms, worst %.1f ms",
                      window_ms / 1000.0, static_cast<unsigned long long>(g_frames),
                      static_cast<unsigned long long>(g_slow_frames), kSlowFrameMs, g_worst_frame / per_ms);
        std::string report = line;

        for (int i = 0; i < kSites; ++i)
        {
            Counter &c = g_counters[i];
            const uint64_t calls = c.calls.exchange(0, std::memory_order_relaxed);
            const uint64_t total = c.total.exchange(0, std::memory_order_relaxed);
            const uint64_t worst = c.worst.exchange(0, std::memory_order_relaxed);
            if (calls == 0)
                continue;
            // Microseconds: a bind is a fraction of one, an upload can be milliseconds.
            std::snprintf(line, sizeof(line), " | %s %llu calls, %.1f ms total, avg %.2f us, worst %.2f ms",
                          kNames[i], static_cast<unsigned long long>(calls), total / per_ms,
                          (total / per_ms) * 1000.0 / static_cast<double>(calls), worst / per_ms);
            report += line;
        }
        Logger::get().debug(report);

        g_window_start = t;
        g_frames = 0;
        g_slow_frames = 0;
        g_worst_frame = 0;
    }
}
