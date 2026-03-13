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
// Transition dựa trên target ± deadband:
//   target   = (temp_min + temp_max) / 2
//   deadband = (temp_max - temp_min) / 4
//
// ※ LOẠI BỎ: Water hysteresis (pump_in/pump_out)
//    Pump In / Pump Out do water_change_manager điều khiển
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

    // Cập nhật config (từ Firebase / Serial)
    void setConfig(const ControlConfig& cfg);

    // Tính toán lệnh relay dựa trên nhiệt độ hiện tại.
    // Điền heater/cooler vào cmd (không đụng các field khác).
    // Trả về ThermalState hiện tại sau khi tính.
    ThermalState compute(const CleanReading& clean, RelayCommand& cmd);

    // Reset về IDLE
    void reset();

    ThermalState  state()    const { return _state; }
    float         target()   const { return _target; }
    float         deadband() const { return _deadband; }

private:
    ThermalState _state;
    float        _target;
    float        _deadband;

    void _updateParams(const ControlConfig& cfg);
};

// Global singleton
extern HysteresisController hysteresisCtrl;
