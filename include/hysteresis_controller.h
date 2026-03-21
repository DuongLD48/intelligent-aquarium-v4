#pragma once
#include "type_definitions.h"
#include "safety_core.h"
#include "control_config.h"

// ================================================================
// hysteresis_controller.h
// Intelligent Aquarium v4.0
//
// State machine 3 trạng thái cho điều khiển nhiệt độ:
//   IDLE    → chờ
//   HEATING → đang sưởi
//   COOLING → đang làm mát
//
// Ngưỡng chuyển trạng thái khớp chính xác với cài đặt user:
//   IDLE → COOLING : T > temp_max
//   IDLE → HEATING : T < temp_min
//   COOLING → IDLE : T <= temp_min   (tắt cooler khi xuống đến ngưỡng dưới)
//   HEATING → IDLE : T >= temp_max   (tắt heater khi lên đến ngưỡng trên)
//
// ※ LOẠI BỎ: Water hysteresis (pump_in/pump_out)
// ================================================================

// ----------------------------------------------------------------
// THERMAL STATE
// ----------------------------------------------------------------
enum class ThermalState : uint8_t {
    IDLE    = 0,
    HEATING = 1,
    COOLING = 2,
};

// ----------------------------------------------------------------
// HYSTERESIS CONTROLLER
// ----------------------------------------------------------------
class HysteresisController {
public:
    HysteresisController();

    void setConfig(const ControlConfig& cfg);

    ThermalState compute(const CleanReading& clean, RelayCommand& cmd);

    void reset();

    ThermalState  state()    const { return _state; }
    float         tempMin()  const { return _temp_min; }
    float         tempMax()  const { return _temp_max; }

    // Compat helpers
    float         target()   const { return (_temp_min + _temp_max) * 0.5f; }
    float         deadband() const { return (_temp_max - _temp_min) * 0.5f; }

private:
    ThermalState _state;
    float        _temp_min;  // ngưỡng dưới — bật heater / tắt cooler
    float        _temp_max;  // ngưỡng trên — bật cooler / tắt heater

    void _updateParams(const ControlConfig& cfg);
};

// Global singleton
extern HysteresisController hysteresisCtrl;