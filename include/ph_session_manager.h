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
//     SAFE_MODE_WAIT   ← kích hoạt chế độ an toàn (tắt tất cả relay)
//       │ warmup_ms hết
//       ▼
//     COLLECTING       ← đọc mẫu pH mỗi 5s
//       │ session xong, có đủ mẫu
//       ▼
//     DOSING           ← tính trung vị → PhDoseController.compute()
//       │ pulse xong (hoặc không cần pulse)
//       ▼
//     IDLE             ← thoát chế độ an toàn, reset timer
//
// Lưu ý:
//   - Trong SAFE_MODE_WAIT và COLLECTING: systemManager.enterSafeMode()
//     giữ tất cả relay tắt, cảm biến nhiệt độ/TDS vẫn đọc bình thường.
//   - Mẫu pH được thu trực tiếp qua readPhOnce() mỗi 5s trong COLLECTING.
//   - Session hợp lệ cần đủ 6 mẫu; sau đó bỏ min/max và tính trên 4 mẫu giữa.
// ================================================================

// ----------------------------------------------------------------
// TRẠNG THÁI SESSION
// ----------------------------------------------------------------
enum class PhSessionState : uint8_t {
    IDLE             = 0,   // Chờ đến lượt đo
    SAFE_MODE_WAIT   = 1,   // Đã vào chế độ an toàn, đang làm ấm (bỏ mẫu)
    COLLECTING       = 2,   // Đang thu mẫu pH hợp lệ
    DOSING           = 3,   // Đang chạy xung bơm
};

// ----------------------------------------------------------------
// BUFFER SESSION pH — tối đa 6 mẫu trong 30s
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

    // Lấy trạng thái hiện tại (để OLED / Firebase hiển thị)
    PhSessionState state() const { return _state; }

    // pH trung vị của session vừa xong (chỉ hợp lệ sau khi update() = true)
    float lastMedianPh() const { return _lastMedianPh; }

    // Kết quả bơm của session vừa xong
    const PhDoseResult& lastDoseResult() const { return _lastDose; }

    // Số giây còn lại đến session tiếp theo (để OLED hiển thị)
    uint32_t secondsUntilNextSession() const;

    // Unix timestamp lần session cuối (cho Firebase ph_session/last_session_ts)
    time_t lastSessionTs() const { return _lastSessionTs; }

    // Số mẫu đã thu trong COLLECTING — dùng cho debug
    uint8_t sampleCount() const { return _sampleCount; }

    // Ép bắt đầu session ngay (từ web/Firebase trigger)
    void triggerNow();

    // Cập nhật cấu hình (từ PhDoseConfig thay đổi runtime)
    void setConfig(const PhDoseConfig& cfg);

    // Debug string
    const char* stateStr() const;

private:
    PhSessionState _state;
    uint32_t       _stateEnteredMs;   // millis() khi vào trạng thái hiện tại

    // Cấu hình (sao chép từ phDoseCtrl)
    PhDoseConfig   _cfg;

    // Buffer mẫu trong pha COLLECTING
    float    _samples[PH_SESSION_MAX_SAMPLES];
    uint8_t  _sampleCount;
    uint32_t _lastSampleMs;  // millis() lần lấy mẫu gần nhất
    float    _sessionLowPassPh;
    bool     _sessionLowPassPrimed;

    // Kết quả session
    float        _lastMedianPh;
    PhDoseResult _lastDose;
    time_t       _lastSessionTs;  // Unix timestamp lần đo cuối

    // Tham chiếu so sánh giữa 2 session — phát hiện biến động đột ngột
    // Chỉ lưu trong RAM — reset khi khởi động lại
    float   _lastMedian;          // NAN = chưa có session nào trước đó

    // ── Bộ đếm streak để tránh false positive ───────────────────
    //
    // _noisyStreak: đếm session NOISY liên tiếp.
    //   >= 3 → _lastMedian bị đóng băng quá lâu → reset về NAN
    //          để session tiếp theo luôn được chấp nhận.
    //
    // _shockStreak: đếm session biến động đột ngột liên tiếp.
    //   == 1 → nghi ngờ, chưa log Firebase, lưu vào _pendingShockMedian
    //   >= 2 → xác nhận biến động thật → log Firebase + cập nhật _lastMedian
    //
    // _pendingShockMedian: trung vị của lần nghi ngờ đầu tiên (chờ xác nhận).
    //   Nếu session sau trở về bình thường → hủy cảnh báo, không log.
    //
    uint8_t _noisyStreak;
    uint8_t _shockStreak;
    float   _pendingShockMedian;

    // Bộ đếm xung bơm pH (non-blocking)
    bool     _pulseActive;
    uint32_t _pulseOffAtMs;
    bool     _phUpActive;
    bool     _phDownActive;

    // Hàm nội bộ
    void   _enterState(PhSessionState s);
    bool   _calcTrimmedStats(float& median, float& spread);
    float  _applySessionLowPass(float rawPh);
    void   _startPulse(bool phUp, bool phDown, uint32_t durationMs);
    void   _tickPulse();
    bool   _isPulseActive() const { return _pulseActive; }
};

// Global singleton
extern PhSessionManager phSessionMgr;
