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

        std::string get_timestamp();
    };
}
