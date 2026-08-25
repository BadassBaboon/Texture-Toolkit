#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <memory>

namespace TextureToolkit
{
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
        void set_min_level(LogLevel level) { m_min_level = level; }

        // Lets a caller skip building an expensive message that would be dropped anyway.
        bool debug_enabled() const { return m_min_level <= LogLevel::Debug; }

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
        LogLevel m_min_level = LogLevel::Info;

        std::string get_timestamp();
    };
}
