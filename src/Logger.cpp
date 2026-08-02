#include "Logger.h"
#include <windows.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace TextureToolkit
{
    Logger &Logger::get()
    {
        static Logger instance;
        return instance;
    }

    Logger::~Logger()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }
    }

    void Logger::init(const std::filesystem::path &log_dir)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return;

        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);

        std::filesystem::path log_path = log_dir / "TextureToolkit.log";
        m_file.open(log_path, std::ios::out | std::ios::trunc);
        if (m_file.is_open())
        {
            m_initialized = true;
            m_file << "[" << get_timestamp() << "] [INFO] [Logger] Texture Toolkit Log Session Started." << std::endl;
        }
    }

    std::string Logger::get_timestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm bt{};
        localtime_s(&bt, &in_time_t);

        std::ostringstream ss;
        ss << std::put_time(&bt, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    void Logger::log(LogLevel level, const std::string &message)
    {
        if (level < m_min_level)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        const char *level_str = "INFO";
        switch (level)
        {
        case LogLevel::Debug:   level_str = "DEBUG"; break;
        case LogLevel::Info:    level_str = "INFO"; break;
        case LogLevel::Warning: level_str = "WARN"; break;
        case LogLevel::Error:   level_str = "ERROR"; break;
        }

        std::string formatted = "[" + get_timestamp() + "] [" + level_str + "] " + message;

        OutputDebugStringA((formatted + "\n").c_str());

        if (m_initialized && m_file.is_open())
        {
            m_file << formatted << std::endl;
        }
    }
}
