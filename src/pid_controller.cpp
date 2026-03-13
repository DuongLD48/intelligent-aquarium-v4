#include "pid_controller.h"
#include "system_constants.h"
#include "logger.h"
#include <math.h>

// ================================================================
// pid_controller.cpp
// Intelligent Aquarium v4.0
// ================================================================

// Global singleton
PidController pidCtrl;

// ----------------------------------------------------------------
PidController::PidController()
    : _kp(1.0f), _ki(0.1f), _kd(0.05f),
      _setpoint(7.0f),
      _prevError(0.0f),
      _integral(0.0f),
      _pulseDurationMs(0),
      _hasLastError(false)
{}

// ----------------------------------------------------------------
void PidController::setConfig(const ControlConfig& cfg) {
    _kp       = cfg.pid_kp;
    _ki       = cfg.pid_ki;
    _kd       = cfg.pid_kd;
    _setpoint = cfg.phSetpoint();

    LOG_INFO("PID", "Config: Kp=%.3f Ki=%.3f Kd=%.3f setpoint=%.2f",
             _kp, _ki, _kd, _setpoint);
}

// ----------------------------------------------------------------
void PidController::reset() {
    _integral      = 0.0f;
    _prevError     = 0.0f;
    _pulseDurationMs = 0;
    _hasLastError  = false;
    LOG_INFO("PID", "Reset");
}

// ================================================================
// COMPUTE — PID pH
// ================================================================
void PidController::compute(const CleanReading& clean, RelayCommand& cmd,
                             uint32_t dt_ms) {
    // Mặc định: tắt cả pH pump
    cmd.ph_up   = false;
    cmd.ph_down = false;
    _pulseDurationMs = 0;

    // Không điều khiển nếu pH không hợp lệ
    if (!clean.ph_valid()) {
        if (_integral != 0.0f) {
            _integral = 0.0f;  // Reset I khi sensor mất
            LOG_WARNING("PID", "pH invalid → reset integral");
        }
        return;
    }

    // dt tối thiểu 1ms để tránh chia 0
    float dt = (dt_ms < 1) ? 0.001f : (float)dt_ms * 0.001f;  // → giây

    // ---- Tính error ----
    float error = _setpoint - clean.ph;

    // ---- Dead zone ----
    if (fabsf(error) < PID_DEADZONE) {
        // pH đủ gần setpoint → không cần action
        // Giữ integral để không tích lũy thêm
        _prevError    = error;
        _hasLastError = true;
        LOG_VERBOSE("PID", "Deadzone: error=%.4f < %.3f", error, PID_DEADZONE);
        return;
    }

    // ---- P term ----
    float p_term = _kp * error;

    // ---- I term (anti-windup: clamp trước khi tích lũy) ----
    _integral += _ki * error * dt;
    // Clamp I term
    if      (_integral >  PID_INTEGRAL_MAX) _integral =  PID_INTEGRAL_MAX;
    else if (_integral < -PID_INTEGRAL_MAX) _integral = -PID_INTEGRAL_MAX;

    // ---- D term ----
    float d_term = 0.0f;
    if (_hasLastError) {
        d_term = _kd * (error - _prevError) / dt;
    }

    // ---- Output (ms) ----
    float output = p_term + _integral + d_term;

    // Clamp tổng output
    if      (output >  PID_MAX_OUTPUT_MS) output =  PID_MAX_OUTPUT_MS;
    else if (output < -PID_MAX_OUTPUT_MS) output = -PID_MAX_OUTPUT_MS;

    // ---- Quyết định relay ----
    // output > 0: pH thấp hơn setpoint → cần tăng pH → bật pH_UP (acid)
    // output < 0: pH cao hơn setpoint  → cần giảm pH → bật pH_DOWN (base)
    if (output > 0.0f) {
        cmd.ph_up        = true;
        cmd.ph_down      = false;
        _pulseDurationMs = (uint32_t)output;
    } else if (output < 0.0f) {
        cmd.ph_up        = false;
        cmd.ph_down      = true;
        _pulseDurationMs = (uint32_t)(-output);
    }

    // Cập nhật state
    _prevError    = error;
    _hasLastError = true;

    LOG_DEBUG("PID", "pH=%.3f sp=%.3f err=%.4f P=%.1f I=%.1f D=%.1f out=%.1fms pump=%s",
              clean.ph, _setpoint, error,
              p_term, _integral, d_term, output,
              cmd.ph_up ? "UP" : (cmd.ph_down ? "DOWN" : "NONE"));
}
