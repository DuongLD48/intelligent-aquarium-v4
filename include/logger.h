#pragma once
#include <Arduino.h>

// ================================================================
// logger.h
// Intelligent Aquarium v4.0
// Log levels + macros. Level có thể thay đổi runtime.
// ================================================================

// ----------------------------------------------------------------
// LOG LEVELS (thứ tự tăng dần về độ chi tiết)
// ----------------------------------------------------------------
enum class LogLevel : uint8_t {
    NONE    = 0,   // Tắt hết
    ERROR   = 1,   // Lỗi nghiêm trọng
    WARNING = 2,   // Cảnh báo
    INFO    = 3,   // Thông tin vận hành
    DEBUG   = 4,   // Debug chi tiết
    VERBOSE = 5    // Rất chi tiết (sensor raw, v.v.)
};

// ----------------------------------------------------------------
// LOGGER — singleton đơn giản
// ----------------------------------------------------------------
class Logger {
public:
    static Logger& instance() {
        static Logger _inst;
        return _inst;
    }

    void init(unsigned long baud = 115200);
    void setLevel(LogLevel level);
    LogLevel getLevel() const { return _level; }

    void log(LogLevel level, const char* tag, const char* fmt, ...);

private:
    Logger() : _level(LogLevel::INFO) {}
    LogLevel _level;

    const char* _levelStr(LogLevel level) const;
};

// ----------------------------------------------------------------
// MACROS — dùng khắp nơi trong project
// Cú pháp: LOG_INFO("TAG", "format %d", value)
// ----------------------------------------------------------------
#define LOG_ERROR(tag, fmt, ...)   Logger::instance().log(LogLevel::ERROR,   tag, fmt, ##__VA_ARGS__)
#define LOG_WARNING(tag, fmt, ...) Logger::instance().log(LogLevel::WARNING, tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)    Logger::instance().log(LogLevel::INFO,    tag, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(tag, fmt, ...)   Logger::instance().log(LogLevel::DEBUG,   tag, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(tag, fmt, ...) Logger::instance().log(LogLevel::VERBOSE, tag, fmt, ##__VA_ARGS__)

// Shortcut: log không có tag
#define LOG_E(fmt, ...) LOG_ERROR("APP",   fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) LOG_WARNING("APP", fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) LOG_INFO("APP",    fmt, ##__VA_ARGS__)
#define LOG_D(fmt, ...) LOG_DEBUG("APP",   fmt, ##__VA_ARGS__)
#define LOG_V(fmt, ...) LOG_VERBOSE("APP", fmt, ##__VA_ARGS__)
