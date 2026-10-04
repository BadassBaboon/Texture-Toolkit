#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <memory>
#include <cstdint>

namespace TextureToolkit
{
    // An address as hex, "0x" included, at the width of a pointer on this build (8 digits on
    // x86, 16 on x64), for log lines.
    inline std::string ptr_hex(const void *p)
    {
        static const char digits[] = "0123456789ABCDEF";
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        std::string out(2 + sizeof(v) * 2, '0');
        out[1] = 'x';
        for (size_t i = out.size(); i > 2; v >>= 4)
            out[--i] = digits[v & 0xF];
        return out;
    }

    enum class LogLevel
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    class Logger
    {
    public:
        static Logger &get();

        void init(const std::filesystem::path &log_dir);
        void log(LogLevel level, const std::string &message);

        // Messages below this level are dropped. Defaults to Info so the very chatty
        // per-texture/per-hook Debug lines don't flood the log (a real perf drain at
        // thousands of lines/sec). Set to Debug via the INI "Verbose" toggle.
        void set_min_level(LogLevel level) { m_min_level.store(level, std::memory_order_relaxed); }

        // Lets a caller skip building an expensive message that would be dropped anyway.
        bool debug_enabled() const { return m_min_level.load(std::memory_order_relaxed) <= LogLevel::Debug; }

        void debug(const std::string &msg) { log(LogLevel::Debug, msg); }
        void info(const std::string &msg) { log(LogLevel::Info, msg); }
        void warn(const std::string &msg) { log(LogLevel::Warning, msg); }
        void error(const std::string &msg) { log(LogLevel::Error, msg); }

    private:
        Logger() = default;
        ~Logger();

        std::mutex m_mutex;
        std::ofstream m_file;
        bool m_initialized = false;
        // Atomic: the panel can switch it while every thread that logs is reading it.
        std::atomic<LogLevel> m_min_level{LogLevel::Info};

        std::string get_timestamp();
    };
}
