#pragma once
#include "analytics.h"
#include <Arduino.h>
#include <esp_task_wdt.h>

// ================================================================
// system_manager.h
// Intelligent Aquarium v4.0
//
// Điều phối khởi động toàn hệ thống theo thứ tự cố định,
// quản lý watchdog và safe mode.
//
// Safe mode triggers:
//   - Mất temp > 60s (fallback_count_temp vượt ngưỡng liên tục)
//   - FSI > 50 liên tục 5 chu kỳ
//
// Safe mode: tắt tất cả relay, vẫn đọc sensor + gửi telemetry
// ================================================================

class SystemManager {
public:
    SystemManager();

    // Khởi tạo toàn bộ hệ thống theo thứ tự:
    // Logger → SafetyCore → ConfigManager → SensorManager →
    // Controllers → WaterChangeManager → Analytics →
    // Watchdog(30s) → WiFi → NTP
    void begin();

    // Gọi đầu mỗi loop():
    //   - Feed watchdog
    //   - Kiểm tra safe mode triggers
    //   - Nếu vào safe mode: gọi emergencyOff()
    void update(const CleanReading& clean, const AnalyticsResult& aResult);

    // Đồng bộ NTP (gọi sau khi WiFi kết nối)
    void syncNtp();

    // bool isSafeMode()  const { return _safeMode; }
    bool isSafeMode() const { return _safeModeBypass ? false : _safeMode; }
    // bool isSafeMode() const;
    bool isNtpSynced() const { return _ntpSynced; }

    void setSafeModeBypass(bool bypass);
    bool isSafeModeBypass() const;


private:
    bool     _safeMode;
    bool     _ntpSynced;
    uint8_t  _highFsiCount;       // Số chu kỳ liên tiếp FSI > 50
    uint32_t _lastNtpAttemptMs;

    static constexpr float    SAFE_MODE_FSI_THRESHOLD  = 50.0f;
    static constexpr uint8_t  SAFE_MODE_FSI_CYCLES     = 5;
    static constexpr uint32_t NTP_RETRY_INTERVAL_MS    = 30000;
    static constexpr uint32_t WATCHDOG_TIMEOUT_S       = 30;

    void _enterSafeMode();
    void _exitSafeMode();
    void _emergencyOff();

    bool _safeModeBypass = false;
};

extern SystemManager systemManager;
