#pragma once

#include <windows.h>
#include <atomic>

#include <string>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <memory>

namespace TextureToolkit
{
    // A name, not the system's description: those are localised, and hex because the decimal form
    // invites misreading E_INVALIDARG as E_OUTOFMEMORY.
    const char *hresult_name(HRESULT hr);

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

        void debug(const std::string &msg) { log(LogLevel::Debug, msg); }
        void info(const std::string &msg) { log(LogLevel::Info, msg); }
        void warn(const std::string &msg) { log(LogLevel::Warning, msg); }
        void error(const std::string &msg) { log(LogLevel::Error, msg); }

        // Sized by a dry run, so no buffer size is guessed and nothing truncates. Not
        // std::format: measured at ~190 KB of extra binary for a handful of diagnostic lines.
        static std::string fmt(_Printf_format_string_ const char *format, ...);


    private:
        Logger() = default;
        ~Logger();

        std::mutex m_mutex;
        std::ofstream m_file;
        bool m_initialized = false;
        std::atomic<LogLevel> m_min_level{LogLevel::Info};

        std::string get_timestamp();
    };
}
