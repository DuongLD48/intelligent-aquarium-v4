#include "hysteresis_controller.h"
#include "logger.h"

// ================================================================
// hysteresis_controller.cpp
// Intelligent Aquarium v4.0
// ================================================================

// Global singleton
HysteresisController hysteresisCtrl;

// ----------------------------------------------------------------
HysteresisController::HysteresisController()
    : _state(ThermalState::IDLE),
      _target(26.5f),
      _deadband(0.75f)
{}

// ----------------------------------------------------------------
void HysteresisController::_updateParams(const ControlConfig& cfg) {
    _target   = cfg.tempTarget();
    _deadband = cfg.tempDeadband();
}

void HysteresisController::setConfig(const ControlConfig& cfg) {
    _updateParams(cfg);
    LOG_INFO("HYSTERESIS", "Config updated: target=%.2f deadband=%.2f", _target, _deadband);
}

// ----------------------------------------------------------------
void HysteresisController::reset() {
    _state = ThermalState::IDLE;
    LOG_INFO("HYSTERESIS", "Reset to IDLE");
}

// ================================================================
// COMPUTE — state machine nhiệt độ
//
// Sơ đồ chuyển trạng thái:
//
//   IDLE ──────── T < (target - deadband) ──────→ HEATING
//   IDLE ──────── T > (target + deadband) ──────→ COOLING
//   HEATING ───── T >= (target + deadband) ─────→ IDLE
//   COOLING ───── T <= (target - deadband) ─────→ IDLE
//
// Hysteresis ngăn relay bật/tắt liên tục (chattering)
// khi nhiệt độ dao động gần setpoint.
// ================================================================
ThermalState HysteresisController::compute(const CleanReading& clean, RelayCommand& cmd) {
    // Nếu temp không hợp lệ → không điều khiển, giữ IDLE
    if (!clean.temp_valid()) {
        cmd.heater = false;
        cmd.cooler = false;
        if (_state != ThermalState::IDLE) {
            _state = ThermalState::IDLE;
            LOG_WARNING("HYSTERESIS", "Temp invalid → IDLE, both OFF");
        }
        return _state;
    }

    float t        = clean.temperature;
    float upper    = _target + _deadband;  // Ngưỡng trên
    float lower    = _target - _deadband;  // Ngưỡng dưới

    ThermalState prev = _state;

    switch (_state) {
        case ThermalState::IDLE:
            if (t < lower) {
                _state = ThermalState::HEATING;
                LOG_INFO("HYSTERESIS", "IDLE→HEATING: T=%.2f < lower=%.2f", t, lower);
            } else if (t > upper) {
                _state = ThermalState::COOLING;
                LOG_INFO("HYSTERESIS", "IDLE→COOLING: T=%.2f > upper=%.2f", t, upper);
            }
            break;

        case ThermalState::HEATING:
            if (t >= upper) {
                _state = ThermalState::IDLE;
                LOG_INFO("HYSTERESIS", "HEATING→IDLE: T=%.2f >= upper=%.2f", t, upper);
            }
            break;

        case ThermalState::COOLING:
            if (t <= lower) {
                _state = ThermalState::IDLE;
                LOG_INFO("HYSTERESIS", "COOLING→IDLE: T=%.2f <= lower=%.2f", t, lower);
            }
            break;
    }

    // Ghi lệnh relay theo state hiện tại
    cmd.heater = (_state == ThermalState::HEATING);
    cmd.cooler = (_state == ThermalState::COOLING);

    // Log chỉ khi state thay đổi
    if (_state != prev) {
        LOG_DEBUG("HYSTERESIS", "State: %d → %d | H=%d C=%d | T=%.2f target=%.2f db=%.2f",
                  (int)prev, (int)_state,
                  cmd.heater, cmd.cooler,
                  t, _target, _deadband);
    }

    return _state;
}
