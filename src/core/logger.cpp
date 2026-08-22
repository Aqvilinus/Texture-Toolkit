#include "core/logger.h"

#include <dxgi.h>
#include <cstdarg>
#include <cstdio>
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
        if (level < m_min_level.load(std::memory_order_relaxed))
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

    std::string Logger::fmt(const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        va_list measure;
        va_copy(measure, args);

        const int needed = std::vsnprintf(nullptr, 0, format, measure);
        va_end(measure);

        std::string out;
        if (needed > 0)
        {
            out.resize(static_cast<size_t>(needed));
            std::vsnprintf(out.data(), static_cast<size_t>(needed) + 1, format, args);
        }
        va_end(args);
        return out;
    }

    // A name, not the system's description: those are localised, and an English log that
    // suddenly speaks another language is worse than a bare code. Hex, because the decimal form
    // invites misreading E_INVALIDARG as E_OUTOFMEMORY.
    const char *hresult_name(HRESULT hr)
    {
        switch (hr)
        {
        case E_INVALIDARG:     return " (E_INVALIDARG)";
        case E_OUTOFMEMORY:    return " (E_OUTOFMEMORY)";
        case E_NOTIMPL:        return " (E_NOTIMPL)";
        case E_FAIL:           return " (E_FAIL)";
        case DXGI_ERROR_DEVICE_REMOVED: return " (DXGI_ERROR_DEVICE_REMOVED)";
        default:               return "";
        }
    }
}
