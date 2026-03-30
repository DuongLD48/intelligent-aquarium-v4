#pragma once
#include "type_definitions.h"
#include "ph_dose_controller.h"
#include "circular_buffer.h"
#include <stdint.h>
#include <time.h>

// ================================================================
// ph_session_manager.h
// Intelligent Aquarium v4.0
//
// Quản lý toàn bộ vòng đời 1 session đo pH:
//
//   State machine:
//     IDLE
//       │ countdown measure_interval_ms hết
//       ▼
//     SAFE_MODE_WAIT   ← kích hoạt safe mode (tắt tất cả relay)
//       │ session_duration_ms hết
//       ▼
//     COLLECTING       ← đọc mẫu pH mỗi 5s, bỏ warmup_ms đầu
//       │ session xong, có đủ mẫu
//       ▼
//     DOSING           ← tính median → PhDoseController.compute()
//       │ pulse xong (hoặc không cần pulse)
//       ▼
//     IDLE             ← thoát safe mode, reset timer
//
// Lưu ý:
//   - Trong SAFE_MODE_WAIT và COLLECTING: systemManager.enterSafeMode()
//     giữ tất cả relay tắt, sensor temp/TDS vẫn đọc bình thường.
//   - pH samples được collect trực tiếp từ rawSensorBuffer mỗi 5s
//     trong phase COLLECTING (sau warmup).
//   - Median được tính từ tối đa 6 mẫu (30s / 5s).
// ================================================================

// ----------------------------------------------------------------
// SESSION STATE
// ----------------------------------------------------------------
enum class PhSessionState : uint8_t {
    IDLE             = 0,   // Chờ đến lượt đo
    SAFE_MODE_WAIT   = 1,   // Đã vào safe mode, đang warm-up (bỏ mẫu)
    COLLECTING       = 2,   // Đang thu mẫu pH hợp lệ
    DOSING           = 3,   // Đang chạy pulse bơm
};

// ----------------------------------------------------------------
// PH SESSION BUFFER — tối đa 6 mẫu trong 30s
// ----------------------------------------------------------------
static constexpr size_t PH_SESSION_MAX_SAMPLES = 6;

// ----------------------------------------------------------------
// PH SESSION MANAGER
// ----------------------------------------------------------------
class PhSessionManager {
public:
    PhSessionManager();

    // Gọi trong setup()
    void begin();

    // Gọi mỗi loop() — state machine chính
    // Trả về true nếu vừa hoàn tất 1 session (có giá trị pH mới)
    bool update();

    // Lấy state hiện tại (để OLED / Firebase hiển thị)
    PhSessionState state() const { return _state; }

    // pH median của session vừa xong (chỉ hợp lệ sau khi update() = true)
    float lastMedianPh() const { return _lastMedianPh; }

    // Kết quả dose của session vừa xong
    const PhDoseResult& lastDoseResult() const { return _lastDose; }

    // Số giây còn lại đến session tiếp theo (để OLED hiển thị)
    uint32_t secondsUntilNextSession() const;

    // Unix timestamp lần session cuối (cho Firebase ph_session/last_session_ts)
    time_t lastSessionTs() const { return _lastSessionTs; }

    // Số mẫu đã thu trong COLLECTING — dùng cho debug print
    uint8_t sampleCount() const { return _sampleCount; }

    // Ép bắt đầu session ngay (từ web/Firebase trigger)
    void triggerNow();

    // Cập nhật config (từ PhDoseConfig thay đổi runtime)
    void setConfig(const PhDoseConfig& cfg);

    // Debug string
    const char* stateStr() const;

private:
    PhSessionState _state;
    uint32_t       _stateEnteredMs;   // millis() khi vào state hiện tại

    // Config (copy từ phDoseCtrl)
    PhDoseConfig   _cfg;

    // Sample buffer trong phase COLLECTING
    float    _samples[PH_SESSION_MAX_SAMPLES];
    uint8_t  _sampleCount;
    uint32_t _lastSampleMs;  // millis() lần lấy mẫu gần nhất

    // Kết quả session
    float        _lastMedianPh;
    PhDoseResult _lastDose;
    time_t       _lastSessionTs;  // Unix timestamp lần đo cuối

    // lastMedian dùng để phát hiện shock giữa 2 session liên tiếp
    // Chỉ lưu trong RAM — reset khi reboot
    float        _lastMedian;     // NAN = chưa có session nào trước đó

    // pH pulse timer (non-blocking, dùng lại logic từ main)
    bool     _pulseActive;
    uint32_t _pulseOffAtMs;
    bool     _phUpActive;
    bool     _phDownActive;

    // Internal helpers
    void   _enterState(PhSessionState s);
    float  _calcMedian();
    void   _startPulse(bool phUp, bool phDown, uint32_t durationMs);
    void   _tickPulse();
    bool   _isPulseActive() const { return _pulseActive; }
};

// Global singleton
extern PhSessionManager phSessionMgr;