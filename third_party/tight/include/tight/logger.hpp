#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace tight {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
};

class Logger {
public:
    static Logger& instance() {
        static Logger s_logger;
        return s_logger;
    }

    void set_level(LogLevel level) { m_level = level; }
    LogLevel level() const { return m_level; }

    void log(LogLevel lv, const std::string& msg) {
        if (lv < m_level) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::string prefix;
        switch (lv) {
        case LogLevel::Debug: prefix = "DEBUG"; break;
        case LogLevel::Info:  prefix = "INFO "; break;
        case LogLevel::Warn:  prefix = "WARN "; break;
        case LogLevel::Error: prefix = "ERROR"; break;
        case LogLevel::Fatal: prefix = "FATAL"; break;
        }
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << prefix << "] " << msg << '\n';
        std::cerr << oss.str();
    }

private:
    LogLevel           m_level{LogLevel::Info};
    std::mutex         m_mutex;
};

#define TIGHT_LOG_DEBUG(msg) \
    tight::Logger::instance().log(tight::LogLevel::Debug, msg)
#define TIGHT_LOG_INFO(msg) \
    tight::Logger::instance().log(tight::LogLevel::Info, msg)
#define TIGHT_LOG_WARN(msg) \
    tight::Logger::instance().log(tight::LogLevel::Warn, msg)
#define TIGHT_LOG_ERROR(msg) \
    tight::Logger::instance().log(tight::LogLevel::Error, msg)

} // namespace tight
