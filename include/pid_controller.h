#pragma once
#include "type_definitions.h"
#include "safety_core.h"
#include "control_config.h"

// ================================================================
// pid_controller.h
// Intelligent Aquarium v4.0
//
// PID controller cho pH:
//   error    = setpoint - measured
//   P        = Kp × error
//   I       += Ki × error × dt  (anti-windup, clamp ±INTEGRAL_MAX)
//   D        = Kd × (error - prev_error) / dt
//   output   = P + I + D  (ms, clamp ±MAX_OUTPUT_MS)
//   deadzone: |error| < DEADZONE → output = 0
//
//   output > 0 → pH_UP pulse (thêm acid → tăng pH)
//   output < 0 → pH_DOWN pulse (thêm base → giảm pH)
//
// Safety Core kiểm tra interval 30s sau đó (check 6).
// ================================================================

// ----------------------------------------------------------------
// PID CONSTANTS
// ----------------------------------------------------------------
static constexpr float PID_DEADZONE      =  0.05f;    // |error| nhỏ hơn này → không action
static constexpr float PID_MAX_OUTPUT_MS = 3000.0f;   // Clamp output ±3000ms
static constexpr float PID_INTEGRAL_MAX  = 2000.0f;   // Anti-windup: clamp I term ±2000ms

// ----------------------------------------------------------------
// PID CONTROLLER
// ----------------------------------------------------------------
class PidController {
public:
    PidController();

    // Cập nhật config (Kp, Ki, Kd, setpoint từ ControlConfig)
    void setConfig(const ControlConfig& cfg);

    // Tính toán lệnh relay pH.
    // Điền ph_up / ph_down + thời gian pulse vào cmd.
    // dt_ms: thời gian ms từ lần tính trước (thường = SENSOR_READ_INTERVAL_MS)
    void compute(const CleanReading& clean, RelayCommand& cmd, uint32_t dt_ms);

    // Đặt thời gian pulse đã tính (để main.cpp tắt relay sau khi hết)
    uint32_t pulseDurationMs() const { return _pulseDurationMs; }

    // Reset tích phân và trạng thái
    void reset();

    // Getters để hiển thị / debug
    float setpoint()  const { return _setpoint; }
    float lastError() const { return _prevError; }
    float integral()  const { return _integral; }

private:
    float    _kp, _ki, _kd;
    float    _setpoint;
    float    _prevError;
    float    _integral;
    uint32_t _pulseDurationMs; // Thời gian pulse output tính ra (ms)
    bool     _hasLastError;
};

// Global singleton
extern PidController pidCtrl;
