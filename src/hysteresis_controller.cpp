#include "hysteresis_controller.h"
#include "logger.h"

// ================================================================
// hysteresis_controller.cpp
// Intelligent Aquarium v4.0
// ================================================================

// ================================================================
// hysteresis_controller.cpp
// Intelligent Aquarium v4.0
// ================================================================

// Global singleton
HysteresisController hysteresisCtrl;

// ----------------------------------------------------------------
HysteresisController::HysteresisController()
    : _state(ThermalState::IDLE),
      _temp_min(25.0f),
      _temp_max(28.0f)
{}

// ----------------------------------------------------------------
void HysteresisController::_updateParams(const ControlConfig& cfg) {
    _temp_min = cfg.temp_min;
    _temp_max = cfg.temp_max;
}

void HysteresisController::setConfig(const ControlConfig& cfg) {
    _updateParams(cfg);
    LOG_INFO("HYSTERESIS", "Config updated: target=%.2f deadband=%.2f",
             target(), deadband());
}

// ----------------------------------------------------------------
void HysteresisController::reset() {
    _state = ThermalState::IDLE;
    LOG_INFO("HYSTERESIS", "Reset to IDLE");
}

// ================================================================
// COMPUTE — state machine nhiệt độ
//
// Ngưỡng khớp chính xác với cài đặt user:
//
//   IDLE ──── T < temp_min ──────────────→ HEATING
//   IDLE ──── T > temp_max ──────────────→ COOLING
//   HEATING ─ T >= temp_max ─────────────→ IDLE   (đủ ấm)
//   COOLING ─ T <= temp_min ─────────────→ IDLE   (đủ mát)
//
// Ví dụ: temp_min=25, temp_max=28, T=28.6
//   → IDLE→COOLING (T > 28)
//   → Cooler chạy đến khi T <= 25 mới tắt
//   → Heater chỉ bật khi T < 25
// ================================================================
ThermalState HysteresisController::compute(const CleanReading& clean, RelayCommand& cmd) {
    // Chỉ tắt heater/cooler khi temp fail liên tục >= stale_sensor_threshold
    // 1-2 lần NaN đơn lẻ (nhiễu OneWire) → giữ nguyên state hiện tại
    uint8_t staleThreshold = safetyCore.getLimits().stale_sensor_threshold;
    if (!clean.temp_valid() && clean.fallback_count_temp >= staleThreshold) {
        cmd.heater = false;
        cmd.cooler = false;
        if (_state != ThermalState::IDLE) {
            _state = ThermalState::IDLE;
            LOG_WARNING("HYSTERESIS", "Temp stale %d/%d cycles → IDLE, both OFF",
                        (int)clean.fallback_count_temp, (int)staleThreshold);
        }
        return _state;
    }

    // Nếu temp chưa valid nhưng chưa đủ threshold → giữ state, giữ relay
    if (!clean.temp_valid()) {
        cmd.heater = (_state == ThermalState::HEATING);
        cmd.cooler = (_state == ThermalState::COOLING);
        return _state;
    }

    float t    = clean.temperature;
    ThermalState prev = _state;

    // Offset nhỏ để tránh bật ngay heater sau khi cooler vừa tắt tại temp_min
    // Ví dụ temp_min=25: cooler tắt tại <=25, heater chỉ bật khi <24.5
    // Offset = 50% của range, tối thiểu 0.3°C
    float range  = _temp_max - _temp_min;
    float offset = (range * 0.5f < 0.3f) ? 0.3f : range * 0.5f;

    switch (_state) {
        case ThermalState::IDLE:
            if (t < (_temp_min - offset)) {
                _state = ThermalState::HEATING;
                LOG_INFO("HYSTERESIS", "IDLE→HEATING: T=%.2f < %.2f (min=%.2f offset=%.2f)",
                         t, _temp_min - offset, _temp_min, offset);
            } else if (t > _temp_max) {
                _state = ThermalState::COOLING;
                LOG_INFO("HYSTERESIS", "IDLE→COOLING: T=%.2f > max=%.2f", t, _temp_max);
            }
            break;

        case ThermalState::HEATING:
            // Tắt heater khi đạt temp_max — phải đi qua IDLE trước khi bật cooler
            if (t >= _temp_max) {
                _state = ThermalState::IDLE;
                LOG_INFO("HYSTERESIS", "HEATING→IDLE: T=%.2f >= max=%.2f", t, _temp_max);
            }
            break;

        case ThermalState::COOLING:
            // Tắt cooler khi về temp_min — phải đi qua IDLE trước khi bật heater
            if (t <= _temp_min) {
                _state = ThermalState::IDLE;
                LOG_INFO("HYSTERESIS", "COOLING→IDLE: T=%.2f <= min=%.2f", t, _temp_min);
            }
            break;
    }

    cmd.heater = (_state == ThermalState::HEATING);
    cmd.cooler = (_state == ThermalState::COOLING);

    if (_state != prev) {
        LOG_DEBUG("HYSTERESIS", "State: %d → %d | H=%d C=%d | T=%.2f [%.2f~%.2f]",
                  (int)prev, (int)_state,
                  cmd.heater, cmd.cooler,
                  t, _temp_min, _temp_max);
    }

    return _state;
}