#include "logger.h"
#include <stdarg.h>
#include <stdio.h>

// ================================================================
// logger.cpp
// Intelligent Aquarium v4.0
// ================================================================

// Kích thước buffer nội bộ để format chuỗi
static constexpr size_t LOG_BUF_SIZE = 256;

// ----------------------------------------------------------------
void Logger::init(unsigned long baud) {
    Serial.begin(baud);
    // Chờ Serial sẵn sàng (ESP32 USB CDC, tối đa 2 giây)
    unsigned long t = millis();
    while (!Serial && (millis() - t < 2000)) {
        delay(10);
    }
    LOG_INFO("LOGGER", "Logger init — level=%s", _levelStr(_level));
}

// ----------------------------------------------------------------
void Logger::setLevel(LogLevel level) {
    _level = level;
    LOG_INFO("LOGGER", "Log level set to %s", _levelStr(level));
}

// ----------------------------------------------------------------
void Logger::log(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level > _level) return;       // Lọc theo level hiện tại
    if (level == LogLevel::NONE) return;

    // Format message
    char buf[LOG_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Timestamp (millis)
    unsigned long ms = millis();
    unsigned long sec  = ms / 1000;
    unsigned long frac = (ms % 1000) / 10;  // 2 chữ số thập phân

    // Output: [LEVEL][TAG] HH:MM:SS.FF message
    // Dùng Serial.printf cho tốc độ
    Serial.printf("[%s][%s] [%lu.%02lu]  %s\n",
                  _levelStr(level),
                  tag,
                  sec,
                  frac,
                  buf);
}

// ----------------------------------------------------------------
const char* Logger::_levelStr(LogLevel level) const {
    switch (level) {
        case LogLevel::ERROR:   return "E";
        case LogLevel::WARNING: return "W";
        case LogLevel::INFO:    return "I";
        case LogLevel::DEBUG:   return "D";
        case LogLevel::VERBOSE: return "V";
        default:                return "?";
    }
}
