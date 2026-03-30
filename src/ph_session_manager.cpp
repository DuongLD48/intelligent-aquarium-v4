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
// ================================================================

// Global singleton
PhSessionManager phSessionMgr;

// ----------------------------------------------------------------
PhSessionManager::PhSessionManager()
    : _state(PhSessionState::IDLE),
      _stateEnteredMs(0),
      _sampleCount(0),
      _lastSampleMs(0),
      _lastMedianPh(7.0f),
      _lastSessionTs(0),
      _lastMedian(NAN),
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

    // Luôn tick pulse timer (active LOW relay)
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
    // Đã kích hoạt safe mode (trong _enterState).
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
    // KHÔNG dùng rawSensorBuffer — buffer đó chứa pH=NAN từ loop bình thường.
    // readPhOnce() đọc ADC trực tiếp khi relay đã tắt (safe mode đang ON).
    // ────────────────────────────────────────────────────────────
    {
        uint32_t collectDuration = _cfg.session_duration_ms - _cfg.warmup_ms;
        uint32_t elapsed         = now - _stateEnteredMs;

        // Lấy mẫu mỗi 5 giây
        if ((now - _lastSampleMs) >= SENSOR_READ_INTERVAL_MS
            && _sampleCount < PH_SESSION_MAX_SAMPLES)
        {
            // Đọc pH trực tiếp từ ADC — relay tắt hết, không có nhiễu
            float ph = readPhOnce();

            if (!isnan(ph) && ph >= 2.0f && ph <= 12.0f) {
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
    // Tính median → dose → thoát safe mode → trả về IDLE.
    // Chạy 1 lần khi vừa vào state, pulse timer xử lý non-blocking.
    // ────────────────────────────────────────────────────────────
    {
        // Chờ pulse hoàn tất (nếu đang chạy)
        if (_isPulseActive()) break;

        // Pulse đã xong (hoặc không có) → về IDLE + thoát safe mode
        LOG_INFO("PHSESS", "DOSING complete → exit safe mode → IDLE");

        // Thoát safe mode để heater/cooler hoạt động lại
        systemManager.exitSafeMode();

        _enterState(PhSessionState::IDLE);
        return true;  // ← Báo main.cpp session hoàn tất
    }

    }  // end switch

    return false;
}

// ================================================================
// ENTER STATE — chuyển state + side effects
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
        // Kích hoạt safe mode: tất cả relay tắt
        systemManager.enterSafeMode();
        LOG_INFO("PHSESS", "→ SAFE_MODE_WAIT (warm-up %lus, safe mode ON)",
                 (unsigned long)(_cfg.warmup_ms / 1000));
        break;

    case PhSessionState::COLLECTING:
        // Reset sample buffer
        _sampleCount  = 0;
        _lastSampleMs = 0;
        memset(_samples, 0, sizeof(_samples));
        LOG_INFO("PHSESS", "→ COLLECTING (max %lus, up to %d samples)",
                 (unsigned long)((_cfg.session_duration_ms - _cfg.warmup_ms) / 1000),
                 (int)PH_SESSION_MAX_SAMPLES);
        break;

    case PhSessionState::DOSING:
    {
        // Tính median
        if (_sampleCount == 0) {
            LOG_WARNING("PHSESS", "DOSING: no samples collected — skip dose");
            _lastMedianPh = NAN;
            _lastDose     = PhDoseResult{};
            // Thoát safe mode ngay
            systemManager.exitSafeMode();
            _state          = PhSessionState::IDLE;
            _stateEnteredMs = millis();
            return;
        }

        // ── Bước 1: Noise check — max-min của _sampleCount mẫu ────
        float sMin = _samples[0], sMax = _samples[0];
        for (uint8_t i = 1; i < _sampleCount; i++) {
            if (_samples[i] < sMin) sMin = _samples[i];
            if (_samples[i] > sMax) sMax = _samples[i];
        }
        float spread = sMax - sMin;

        if (spread > _cfg.noise_threshold) {
            // NOISY — log Firebase ph_session/sensor_error, skip dose → IDLE
            LOG_WARNING("PHSESS", "NOISY: spread=%.3f > threshold=%.3f — skip dose",
                        spread, _cfg.noise_threshold);

            firebaseClient.logPhSensorError(spread, _cfg.noise_threshold, _sampleCount);

            _lastDose = PhDoseResult{};
            systemManager.exitSafeMode();
            _state          = PhSessionState::IDLE;
            _stateEnteredMs = millis();
            return;
        }

        // ── Bước 2: Tính median ─────────────────────────────────────
        float median = _calcMedian();
        _lastSessionTs = time(nullptr);

        LOG_INFO("PHSESS", "→ DOSING: %d samples, spread=%.3f, median pH=%.3f",
                 _sampleCount, spread, median);

        // ── Bước 3: Shock check — so với session trước ─────────────
        if (!isnan(_lastMedian)) {
            float delta = fabsf(median - _lastMedian);
            if (delta > _cfg.shock_threshold) {
                // SHOCK — log Firebase history/shock_event_ph, skip dose → IDLE
                LOG_WARNING("PHSESS", "SHOCK: |%.3f - %.3f| = %.3f > threshold=%.3f — skip dose",
                            median, _lastMedian, delta, _cfg.shock_threshold);

                firebaseClient.logPhShockEvent(_lastMedian, median, delta);

                _lastMedianPh = median;  // cập nhật để OLED/Firebase hiển thị
                _lastDose     = PhDoseResult{};
                systemManager.exitSafeMode();
                _state          = PhSessionState::IDLE;
                _stateEnteredMs = millis();
                return;
            }
        }

        // ── Bước 4: Accept — cập nhật _lastMedian, tiến hành dose ─
        _lastMedian   = median;
        _lastMedianPh = median;

        // Gọi PhDoseController
        _lastDose = phDoseCtrl.compute(_lastMedianPh);

        // Khởi động pulse nếu có action
        if ((_lastDose.ph_up || _lastDose.ph_down) && _lastDose.pulse_ms > 0) {
            uint32_t maxPulse = safetyCore.getLimits().ph_pump_max_pulse_ms;
            uint32_t dur      = _lastDose.pulse_ms > maxPulse ? maxPulse : _lastDose.pulse_ms;

            // Check ph_pump_min_interval trước khi bật relay
            if (safetyCore.checkPhPumpAllowed()) {
                _startPulse(_lastDose.ph_up, _lastDose.ph_down, dur);
            } else {
                LOG_WARNING("PHSESS", "DOSING: pH pump blocked by safety interval — skip pulse");
                _lastDose.pulse_ms = 0;  // Clear để Firebase biết không có pulse
            }
        } else {
            LOG_INFO("PHSESS", "DOSING: deadzone — no pulse needed");
        }
        break;
    }

    }
}

// ================================================================
// CALC MEDIAN — sort in-place trên bản sao, trả về giá trị giữa
// ================================================================
float PhSessionManager::_calcMedian() {
    if (_sampleCount == 0) return NAN;
    if (_sampleCount == 1) return _samples[0];

    // Sao chép để không làm hỏng _samples
    float tmp[PH_SESSION_MAX_SAMPLES];
    memcpy(tmp, _samples, _sampleCount * sizeof(float));

    // Sort bong bóng đơn giản (max 6 phần tử)
    for (uint8_t i = 0; i < _sampleCount - 1; i++) {
        for (uint8_t j = 0; j < _sampleCount - 1 - i; j++) {
            if (tmp[j] > tmp[j + 1]) {
                float t = tmp[j]; tmp[j] = tmp[j + 1]; tmp[j + 1] = t;
            }
        }
    }

    // Median: giữa nếu lẻ, trung bình 2 giữa nếu chẵn
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

    // Bật relay (active LOW = digitalWrite LOW)
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