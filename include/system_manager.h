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
// Safe mode triggers (tự động):
//   - Temp + pH cùng mất (fallback_count >= stale_threshold * 2)
//   - FSI > 50 liên tục 5 chu kỳ
//
// Safe mode triggers (thủ công):
//   - PhSessionManager gọi enterSafeMode() trước khi đo pH
//
// Safe mode exit:
//   - Tự động: khi temp HOẶC pH phục hồi / FSI bình thường
//   - Thủ công: PhSessionManager gọi exitSafeMode() sau khi đo xong
//
// Safe mode: tắt tất cả relay, vẫn đọc sensor + gửi telemetry
// ================================================================

class SystemManager {
public:
    SystemManager();

    // Khởi tạo toàn bộ hệ thống theo thứ tự cố định
    void begin();

    // Gọi đầu mỗi loop(): feed watchdog + kiểm tra safe mode triggers
    void update(const CleanReading& clean, const AnalyticsResult& aResult);

    // Đồng bộ NTP (gọi sau khi WiFi kết nối)
    void syncNtp();

    bool isSafeMode()   const { return _safeModeBypass ? false : _safeMode; }
    bool isNtpSynced()  const { return _ntpSynced; }

    void setSafeModeBypass(bool bypass);
    bool isSafeModeBypass() const;

    // ---- Public safe mode control (cho PhSessionManager) ----
    // Kích hoạt safe mode thủ công: tắt tất cả relay.
    // Nếu đã ở safe mode → không làm gì thêm.
    void enterSafeMode();

    // Thoát safe mode thủ công (chỉ khi không có sensor-triggered safe mode).
    // Nếu system vẫn cần safe mode (sensor lỗi) → giữ nguyên.
    void exitSafeMode();

private:
    bool     _safeMode;
    bool     _ntpSynced;
    bool     _multiSensorLost;
    uint8_t  _highFsiCount;
    uint32_t _lastNtpAttemptMs;
    uint32_t _lastNtpSyncMs;
    bool     _lastWifiState;

    // Đánh dấu safe mode do PhSession kích hoạt (không phải sensor)
    // Để exitSafeMode() biết có được phép thoát không
    bool _sessionSafeMode;

    static constexpr float    SAFE_MODE_FSI_THRESHOLD  = 50.0f;
    static constexpr uint8_t  SAFE_MODE_FSI_CYCLES     = 5;
    static constexpr uint32_t NTP_RETRY_INTERVAL_MS    = 30000;
    static constexpr uint32_t NTP_RESYNC_INTERVAL_MS   = 6UL * 3600UL * 1000UL;
    static constexpr uint32_t WATCHDOG_TIMEOUT_S       = 30;
    static constexpr uint32_t MIN_FREE_HEAP_BYTES       = 30000;

    void _enterSafeMode();
    void _exitSafeMode();
    void _emergencyOff();

    bool _safeModeBypass = false;
};

extern SystemManager systemManager;
