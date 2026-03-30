#pragma once
#include "type_definitions.h"
#include "circular_buffer.h"
#include "system_constants.h"
#include <stdint.h>

// ================================================================
// analytics.h
// Intelligent Aquarium v4.0
//
// Phân tích xu hướng và ổn định dữ liệu sensor sau pipeline:
//
//   EMA   — Exponential Moving Average (xu hướng trơn)
//   CUSUM — Cumulative Sum (phát hiện drift dài hạn)
//   WSI   — Water Stability Index (0–100, càng cao càng ổn định)
//   FSI   — Fluctuation Severity Index (càng cao càng biến động)
//
// Gọi analytics.update(cleanBuffer) mỗi 5 giây sau khi push
// CleanReading mới vào cleanBuffer.
// ================================================================

// ----------------------------------------------------------------
// ANALYTICS CONFIG — có thể thay đổi runtime
// ----------------------------------------------------------------
struct AnalyticsConfig {
    // EMA
    float ema_alpha = 0.1f;         // Hệ số làm mịn [0.01, 0.5]

    // CUSUM
    float cusum_k          = 0.5f;  // Slack — độ nhạy
    float cusum_threshold  = 5.0f;  // Ngưỡng báo drift

    // WSI weights (tổng = 1.0) — pH bỏ, temp+tds bù
    float wsi_weight_temp  = 0.6f;
    float wsi_weight_tds   = 0.4f;

    // FSI coefficients
    float fsi_alpha        = 0.5f;  // Trọng số |ΔT|
    float fsi_shock_penalty = 20.0f; // Cộng thêm mỗi shock event

    // Window để tính std/mean cho WSI (dùng cleanBuffer trực tiếp)
    size_t wsi_window = 12;         // 12 mẫu × 5s = 60 giây
};

// ----------------------------------------------------------------
// DRIFT DIRECTION
// ----------------------------------------------------------------
enum class DriftDir : uint8_t {
    NONE  = 0,
    UP    = 1,  // Đang tăng dài hạn
    DOWN  = 2,  // Đang giảm dài hạn
};

// ----------------------------------------------------------------
// EMA STATE — lưu giá trị EMA hiện tại cho 3 field
// ----------------------------------------------------------------
struct EmaState {
    float temp = 25.0f;
    float tds  = 200.0f;
    bool  initialized = false;
};

// ----------------------------------------------------------------
// CUSUM STATE — lưu accumulator cho mỗi field × 2 hướng
// ----------------------------------------------------------------
struct CusumState {
    float s_up   = 0.0f;  // Tích lũy hướng tăng
    float s_down = 0.0f;  // Tích lũy hướng giảm
    DriftDir drift = DriftDir::NONE;
};

// ----------------------------------------------------------------
// ANALYTICS RESULT — output mỗi chu kỳ
// ----------------------------------------------------------------
struct AnalyticsResult {
    // EMA
    float ema_temp = 25.0f;
    float ema_tds  = 200.0f;

    // CUSUM drift
    DriftDir drift_temp = DriftDir::NONE;
    DriftDir drift_tds  = DriftDir::NONE;

    // WSI: 0 (không ổn định) → 100 (rất ổn định)
    float wsi = 100.0f;

    // FSI: 0 (bình lặng) → cao (biến động mạnh)
    float fsi = 0.0f;

    // Có bất kỳ drift nào không
    bool hasDrift() const {
        return drift_temp != DriftDir::NONE ||
               drift_tds  != DriftDir::NONE;
    }
};

// ----------------------------------------------------------------
// ANALYTICS CLASS
// ----------------------------------------------------------------
class Analytics {
public:
    Analytics();

    // Cập nhật toàn bộ analytics sau khi có CleanReading mới
    // cleanBuf: toàn bộ lịch sử clean readings
    void update(const CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE>& cleanBuf);

    // Cập nhật config runtime
    void setConfig(const AnalyticsConfig& cfg);
    const AnalyticsConfig& getConfig() const { return _cfg; }

    // Kết quả mới nhất
    const AnalyticsResult& result() const { return _result; }

    // Reset tất cả state
    void reset();

private:
    AnalyticsConfig  _cfg;
    AnalyticsResult  _result;

    // EMA state cho 2 field
    EmaState _ema;

    // CUSUM state cho 2 field
    CusumState _cusumTemp;
    CusumState _cusumTds;

    // Giá trị CleanReading trước (cho FSI delta)
    float    _prevTemp;
    bool     _hasPrev;
    uint32_t _shockCount;  // Tổng shock trong window gần nhất

    // --- Private helpers ---
    void _updateEma(const CleanReading& latest);
    void _updateCusum(const CleanReading& latest);
    void _updateWsi(const CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE>& buf);
    void _updateFsi(const CleanReading& latest);

    // Tính CUSUM cho 1 field, cập nhật CusumState
    DriftDir _computeCusum(float value, float ema, CusumState& state);

    // Tính std / mean từ buffer window (dùng cho WSI)
    // Trả về CV = std/mean (coefficient of variation), 0 nếu không đủ data
    float _coeffOfVariation(
        const CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE>& buf,
        size_t window,
        float CleanReading::*field
    ) const;
};

// Global singleton
extern Analytics analytics;
