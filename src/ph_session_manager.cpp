#include "ph_session_manager.h"
#include "data_pipeline.h"
#include "ph_dose_controller.h"
#include "system_manager.h"
#include "safety_core.h"
#include "sensor_manager.h"
#include "wifi_firebase.h"
#include "logger.h"
#include "system_constants.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <algorithm>

// ================================================================
// ph_session_manager.cpp
// Intelligent Aquarium v4.0
//
// State machine vòng đời session đo pH:
//   IDLE → SAFE_MODE_WAIT → COLLECTING → DOSING → IDLE
//
// [FIX v4.1] Sửa bug _lastMedian bị đóng băng gây báo biến động
//            đột ngột (shock) liên tiếp sai:
//   - NOISY streak >= 3 → reset _lastMedian về NAN (xả đóng băng)
//   - Biến động đột ngột lần 1 → chưa log Firebase, chờ xác nhận
//   - Biến động đột ngột lần 2 liên tiếp → xác nhận thật, log + cập nhật _lastMedian
//   - pH hồi phục giữa chừng → hủy cảnh báo, chấp nhận bình thường
// ================================================================

// Global singleton
PhSessionManager phSessionMgr;

// ----------------------------------------------------------------
PhSessionManager::PhSessionManager()
    : _state(PhSessionState::IDLE),
      _stateEnteredMs(0),
      _sampleCount(0),
      _lastSampleMs(0),
      _lastMedianPh(NAN),
      _lastSessionTs(0),
      _lastMedian(NAN),
      _noisyStreak(0),
      _shockStreak(0),
      _pendingShockMedian(NAN),
      _pulseActive(false),
      _pulseOffAtMs(0),
      _phUpActive(false),
      _phDownActive(false)
{
    memset(_samples, 0, sizeof(_samples));
}

// ----------------------------------------------------------------
void PhSessionManager::begin() {
    _cfg = phDoseCtrl.getDoseConfig();
    _enterState(PhSessionState::IDLE);
    LOG_INFO("PHSESS", "begin — interval=%lus session=%lus warmup=%lus",
             (unsigned long)(_cfg.measure_interval_ms / 1000),
             (unsigned long)(_cfg.session_duration_ms / 1000),
             (unsigned long)(_cfg.warmup_ms           / 1000));
}

// ----------------------------------------------------------------
void PhSessionManager::setConfig(const PhDoseConfig& cfg) {
    _cfg = cfg;
    LOG_INFO("PHSESS", "Config updated: interval=%lus session=%lus warmup=%lus",
             (unsigned long)(_cfg.measure_interval_ms / 1000),
             (unsigned long)(_cfg.session_duration_ms / 1000),
             (unsigned long)(_cfg.warmup_ms           / 1000));
}

// ----------------------------------------------------------------
void PhSessionManager::triggerNow() {
    if (_state != PhSessionState::IDLE) {
        LOG_WARNING("PHSESS", "triggerNow ignored — already in session (state=%s)", stateStr());
        return;
    }
    LOG_INFO("PHSESS", "Manual trigger → starting session now");
    _enterState(PhSessionState::SAFE_MODE_WAIT);
}

// ----------------------------------------------------------------
uint32_t PhSessionManager::secondsUntilNextSession() const {
    if (_state != PhSessionState::IDLE) return 0;
    uint32_t elapsed = millis() - _stateEnteredMs;
    if (elapsed >= _cfg.measure_interval_ms) return 0;
    return (_cfg.measure_interval_ms - elapsed) / 1000UL;
}

// ================================================================
// UPDATE — gọi mỗi loop()
// Trả về true khi session vừa hoàn tất (có pH mới và dose đã thực thi)
// ================================================================
bool PhSessionManager::update() {
    uint32_t now = millis();

    // Luôn tick bộ đếm xung (relay active LOW)
    _tickPulse();

    switch (_state) {

    // ────────────────────────────────────────────────────────────
    case PhSessionState::IDLE:
    // ────────────────────────────────────────────────────────────
    {
        uint32_t elapsed = now - _stateEnteredMs;
        if (elapsed >= _cfg.measure_interval_ms) {
            LOG_INFO("PHSESS", "Interval elapsed (%lus) → starting pH session",
                     (unsigned long)(_cfg.measure_interval_ms / 1000));
            _enterState(PhSessionState::SAFE_MODE_WAIT);
        }
        break;
    }

    // ────────────────────────────────────────────────────────────
    case PhSessionState::SAFE_MODE_WAIT:
    // Đã kích hoạt chế độ an toàn (trong _enterState).
    // Bỏ toàn bộ mẫu trong warmup_ms đầu → không lấy gì.
    // Sau warmup_ms → chuyển sang COLLECTING.
    // ────────────────────────────────────────────────────────────
    {
        uint32_t elapsed = now - _stateEnteredMs;
        if (elapsed >= _cfg.warmup_ms) {
            LOG_INFO("PHSESS", "Warm-up done (%lus) → COLLECTING",
                     (unsigned long)(_cfg.warmup_ms / 1000));
            _enterState(PhSessionState::COLLECTING);
        }
        break;
    }

    // ────────────────────────────────────────────────────────────
    case PhSessionState::COLLECTING:
    // Đọc pH trực tiếp từ ADC mỗi 5 giây.
    // KHÔNG dùng rawSensorBuffer — buffer đó chứa pH=NAN từ vòng lặp bình thường.
    // readPhOnce() đọc ADC trực tiếp khi relay đã tắt (chế độ an toàn đang ON).
    // ────────────────────────────────────────────────────────────
    {
        uint32_t collectDuration = _cfg.session_duration_ms - _cfg.warmup_ms;
        uint32_t elapsed         = now - _stateEnteredMs;

        // Lấy mẫu mỗi 5 giây
        if ((now - _lastSampleMs) >= SENSOR_READ_INTERVAL_MS
            && _sampleCount < PH_SESSION_MAX_SAMPLES)
        {
            float ph = readPhOnce();

            if (!isnan(ph) && ph >= 3.0f && ph <= 10.0f) {
                _samples[_sampleCount++] = ph;
                LOG_INFO("PHSESS", "Sample[%d/%d] = %.3f (direct ADC, relay OFF)",
                         _sampleCount - 1, (int)PH_SESSION_MAX_SAMPLES, ph);
            } else {
                LOG_WARNING("PHSESS", "Sample invalid (ph=%.3f) — skipped", ph);
            }
            _lastSampleMs = now;
        }

        // Kết thúc COLLECTING khi hết thời gian hoặc đầy buffer
        bool timeUp     = (elapsed >= collectDuration);
        bool bufferFull = (_sampleCount >= PH_SESSION_MAX_SAMPLES);

        if (timeUp || bufferFull) {
            LOG_INFO("PHSESS", "COLLECTING done: %d samples in %lus (timeUp=%d bufFull=%d)",
                     _sampleCount, (unsigned long)(elapsed / 1000), timeUp, bufferFull);
            _enterState(PhSessionState::DOSING);
        }
        break;
    }

    // ────────────────────────────────────────────────────────────
    case PhSessionState::DOSING:
    // Chờ xung hoàn tất (non-blocking). Logic tính toán và quyết định
    // bơm được thực hiện 1 lần duy nhất trong _enterState(DOSING).
    // ────────────────────────────────────────────────────────────
    {
        // Chờ xung hoàn tất (nếu đang chạy)
        if (_isPulseActive()) break;

        // Xung đã xong (hoặc không có) → về IDLE + thoát chế độ an toàn
        LOG_INFO("PHSESS", "DOSING complete → exit safe mode → IDLE");
        systemManager.exitSafeMode();
        firebaseClient.clearPhSensorErrorFlag();
        _enterState(PhSessionState::IDLE);
        return true;
    }

    }  // end switch

    return false;
}

// ================================================================
// ENTER STATE — chuyển trạng thái + xử lý side effects
// Với DOSING: toàn bộ logic kiểm tra nhiễu, biến động, bơm chạy ở đây
// ================================================================
void PhSessionManager::_enterState(PhSessionState s) {
    _state          = s;
    _stateEnteredMs = millis();

    switch (s) {

    case PhSessionState::IDLE:
        LOG_INFO("PHSESS", "→ IDLE (next session in %lus)",
                 (unsigned long)(_cfg.measure_interval_ms / 1000));
        break;

    case PhSessionState::SAFE_MODE_WAIT:
        systemManager.enterSafeMode();
        LOG_INFO("PHSESS", "→ SAFE_MODE_WAIT (warm-up %lus, chế độ an toàn ON)",
                 (unsigned long)(_cfg.warmup_ms / 1000));
        break;

    case PhSessionState::COLLECTING:
        _sampleCount  = 0;
        _lastSampleMs = 0;
        memset(_samples, 0, sizeof(_samples));
        LOG_INFO("PHSESS", "→ COLLECTING (max %lus, up to %d samples)",
                 (unsigned long)((_cfg.session_duration_ms - _cfg.warmup_ms) / 1000),
                 (int)PH_SESSION_MAX_SAMPLES);
        break;

    case PhSessionState::DOSING:
    {
        // ── Không có mẫu → thoát sớm ───────────────────────────────
        if (_sampleCount == 0) {
            LOG_WARNING("PHSESS", "DOSING: no samples collected (all out of range) — sensor error");
            firebaseClient.logPhSensorError(NAN, NAN, 0);  // Indicate all samples out of range
            _lastMedianPh = NAN;
            _lastDose     = PhDoseResult{};
            systemManager.exitSafeMode();
            _state          = PhSessionState::IDLE;
            _stateEnteredMs = millis();
            return;
        }

        // ── Bước 1: Kiểm tra nhiễu — max-min của các mẫu ──────────
        float sMin = _samples[0], sMax = _samples[0];
        for (uint8_t i = 1; i < _sampleCount; i++) {
            if (_samples[i] < sMin) sMin = _samples[i];
            if (_samples[i] > sMax) sMax = _samples[i];
        }
        float spread = sMax - sMin;

        if (spread > _cfg.noise_threshold) {
            // NOISY — cảm biến nhiễu, không có dữ liệu đáng tin
            _noisyStreak++;
            _shockStreak        = 0;    // NOISY phá vỡ chuỗi nghi ngờ đang chờ
            _pendingShockMedian = NAN;

            LOG_WARNING("PHSESS",
                "NOISY: spread=%.3f > threshold=%.3f — skip dose (noisyStreak=%d)",
                spread, _cfg.noise_threshold, (int)_noisyStreak);

            firebaseClient.logPhSensorError(spread, _cfg.noise_threshold, _sampleCount);

            // NOISY kéo dài >= 3 session liên tiếp → _lastMedian bị đóng băng quá lâu
            // → reset về NAN để session tiếp theo luôn được chấp nhận (tránh báo động sai)
            if (_noisyStreak >= 3) {
                LOG_WARNING("PHSESS",
                    "NOISY streak=%d >= 3 → reset _lastMedian (giải phóng tham chiếu đóng băng)",
                    (int)_noisyStreak);
                _lastMedian  = NAN;
                _noisyStreak = 0;
            }

            _lastDose = PhDoseResult{};
            systemManager.exitSafeMode();
            _state          = PhSessionState::IDLE;
            _stateEnteredMs = millis();
            return;
        }

        // ── Bước 2: Tính trung vị ──────────────────────────────────
        float median   = _calcMedian();
        _lastSessionTs = time(nullptr);
        _noisyStreak   = 0;  // Session hợp lệ → reset bộ đếm nhiễu

        LOG_INFO("PHSESS", "→ DOSING: %d samples, spread=%.3f, median pH=%.3f",
                 _sampleCount, spread, median);

        // ── Bước 3: Kiểm tra biến động đột ngột — so với session trước
        if (!isnan(_lastMedian)) {
            float delta = fabsf(median - _lastMedian);
            if (delta > _cfg.shock_threshold) {
                _shockStreak++;

                if (_shockStreak == 1) {
                    // Lần nghi ngờ đầu tiên: chưa log Firebase, chờ xác nhận.
                    // Có thể là nhiễu ADC thoáng qua, không phải biến động thật.
                    _pendingShockMedian = median;
                    _lastMedianPh       = median;  // cập nhật hiển thị OLED/dashboard

                    LOG_WARNING("PHSESS",
                        "Nghi ngờ biến động đột ngột lần 1: |%.3f - %.3f| = %.3f > %.3f — chờ xác nhận",
                        median, _lastMedian, delta, _cfg.shock_threshold);

                } else {
                    // Lần 2 trở đi: xác nhận biến động thật → log Firebase
                    // ph_before dùng _lastMedian gốc (trước khi biến động bắt đầu)
                    float phBefore = _lastMedian;

                    LOG_WARNING("PHSESS",
                        "Biến động đột ngột XÁC NHẬN (streak=%d): %.3f → %.3f — log Firebase",
                        (int)_shockStreak, phBefore, median);

                    firebaseClient.logPhShockEvent(phBefore, median, delta);

                    // Cập nhật tham chiếu về giá trị pH thực tế hiện tại
                    _lastMedian         = median;
                    _lastMedianPh       = median;
                    _pendingShockMedian = NAN;
                    _shockStreak        = 0;
                }

                _lastDose = PhDoseResult{};
                systemManager.exitSafeMode();
                _state          = PhSessionState::IDLE;
                _stateEnteredMs = millis();
                return;
            }
        }

        // pH trở về bình thường sau khi đang nghi ngờ → hủy cảnh báo
        if (_shockStreak > 0) {
            LOG_INFO("PHSESS",
                "pH hồi phục sau %d session nghi ngờ (%.3f → %.3f) — hủy cảnh báo, chấp nhận",
                (int)_shockStreak,
                isnan(_pendingShockMedian) ? 0.0f : _pendingShockMedian,
                median);
            _shockStreak        = 0;
            _pendingShockMedian = NAN;
        }

        // ── Bước 4: Chấp nhận — cập nhật _lastMedian, tiến hành bơm
        _lastMedian   = median;
        _lastMedianPh = median;

        _lastDose = phDoseCtrl.compute(_lastMedianPh);

        if ((_lastDose.ph_up || _lastDose.ph_down) && _lastDose.pulse_ms > 0) {
            uint32_t maxPulse = safetyCore.getLimits().ph_pump_max_pulse_ms;
            uint32_t dur      = _lastDose.pulse_ms > maxPulse ? maxPulse : _lastDose.pulse_ms;

            if (safetyCore.checkPhPumpAllowed()) {
                _startPulse(_lastDose.ph_up, _lastDose.ph_down, dur);
            } else {
                LOG_WARNING("PHSESS", "DOSING: bơm pH bị chặn bởi khoảng an toàn — skip pulse");
                _lastDose.pulse_ms = 0;
            }
        } else {
            LOG_INFO("PHSESS", "DOSING: pH trong vùng hợp lệ — không cần bơm");
        }
        break;
    }

    }  // end switch _enterState
}

// ================================================================
// CALC MEDIAN — sort in-place trên bản sao, trả về giá trị giữa
// ================================================================
float PhSessionManager::_calcMedian() {
    if (_sampleCount == 0) return NAN;
    if (_sampleCount == 1) return _samples[0];

    float tmp[PH_SESSION_MAX_SAMPLES];
    memcpy(tmp, _samples, _sampleCount * sizeof(float));

    // Sắp xếp nổi bọt đơn giản (tối đa 6 phần tử)
    for (uint8_t i = 0; i < _sampleCount - 1; i++) {
        for (uint8_t j = 0; j < _sampleCount - 1 - i; j++) {
            if (tmp[j] > tmp[j + 1]) {
                float t = tmp[j]; tmp[j] = tmp[j + 1]; tmp[j + 1] = t;
            }
        }
    }

    if (_sampleCount % 2 == 1) {
        return tmp[_sampleCount / 2];
    } else {
        return (tmp[_sampleCount / 2 - 1] + tmp[_sampleCount / 2]) * 0.5f;
    }
}

// ================================================================
// PULSE TIMER — non-blocking, ghi GPIO trực tiếp
// ================================================================
void PhSessionManager::_startPulse(bool phUp, bool phDown, uint32_t durationMs) {
    if (durationMs == 0) return;
    _pulseActive  = true;
    _phUpActive   = phUp;
    _phDownActive = phDown;
    _pulseOffAtMs = millis() + durationMs;

    if (phUp)   digitalWrite(PIN_RELAY_PH_UP,   LOW);
    if (phDown) digitalWrite(PIN_RELAY_PH_DOWN, LOW);

    LOG_INFO("PHSESS", "Pulse start: %s dur=%lums",
             phUp ? "pH_UP" : "pH_DOWN",
             (unsigned long)durationMs);
}

void PhSessionManager::_tickPulse() {
    if (!_pulseActive) return;
    if (millis() >= _pulseOffAtMs) {
        if (_phUpActive)   digitalWrite(PIN_RELAY_PH_UP,   HIGH);  // OFF
        if (_phDownActive) digitalWrite(PIN_RELAY_PH_DOWN, HIGH);  // OFF
        _pulseActive  = false;
        _phUpActive   = false;
        _phDownActive = false;
        LOG_INFO("PHSESS", "Pulse ended");
    }
}

// ----------------------------------------------------------------
const char* PhSessionManager::stateStr() const {
    switch (_state) {
        case PhSessionState::IDLE:           return "IDLE";
        case PhSessionState::SAFE_MODE_WAIT: return "SAFE_MODE_WAIT";
        case PhSessionState::COLLECTING:     return "COLLECTING";
        case PhSessionState::DOSING:         return "DOSING";
        default:                             return "UNKNOWN";
    }
}
