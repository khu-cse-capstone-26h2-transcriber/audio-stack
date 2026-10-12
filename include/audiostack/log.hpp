// ============================================================================
// Logging helper. stderr output with timestamps. Shared by daemon and tools.
// ============================================================================
#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>

namespace astk {

enum class LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance() { static Logger l; return l; }

    void set_level(LogLevel lv) { level_ = lv; }
    void set_level(const std::string& s) {
        if (s == "trace") level_ = LogLevel::TRACE;
        else if (s == "debug") level_ = LogLevel::DEBUG;
        else if (s == "info") level_ = LogLevel::INFO;
        else if (s == "warn") level_ = LogLevel::WARN;
        else if (s == "error") level_ = LogLevel::ERROR;
    }
    void set_ident(const std::string& id) { ident_ = id; }

    void log(LogLevel lv, const char* fmt, ...) {
        if (lv < level_) return;
        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);

        struct timespec ts {};
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm {};
        gmtime_r(&ts.tv_sec, &tm);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);

        std::fprintf(stderr, "%s.%03ld [%s] %s: %s\n", stamp,
                     long(ts.tv_nsec / 1000000), tag(lv), ident_.c_str(), msg);
    }

private:
    LogLevel level_ {LogLevel::INFO};
    std::string ident_ {"audiostack"};
    static const char* tag(LogLevel lv) {
        switch (lv) {
            case LogLevel::TRACE: return "TRC";
            case LogLevel::DEBUG: return "DBG";
            case LogLevel::INFO:  return "INF";
            case LogLevel::WARN:  return "WRN";
            case LogLevel::ERROR: return "ERR";
        }
        return "?";
    }
};

#define ASTK_LOG(lv, ...) ::astk::Logger::instance().log(lv, __VA_ARGS__)
#define LOG_TRACE(...) ASTK_LOG(::astk::LogLevel::TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) ASTK_LOG(::astk::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  ASTK_LOG(::astk::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...)  ASTK_LOG(::astk::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) ASTK_LOG(::astk::LogLevel::ERROR, __VA_ARGS__)

} // namespace astk
