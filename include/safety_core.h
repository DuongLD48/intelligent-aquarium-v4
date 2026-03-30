#pragma once
#include "type_definitions.h"
#include <stdint.h>
#include <Arduino.h>

// ================================================================
// safety_core.h
// Intelligent Aquarium v4.0
//
// Kiểm tra an toàn 7 tầng tuần tự trước khi ghi relay vật lý.
// Tích hợp SafetyLimits runtime (blueprint bước 17b) để Admin
// có thể cập nhật qua Firebase mà không cần flash lại firmware.
//
// ※ LOẠI BỎ: water level check, WATER_CRITICAL_LOW_CM
// ================================================================

// ----------------------------------------------------------------
// RELAY INDEX — ánh xạ enum → GPIO
// ----------------------------------------------------------------
enum class RelayIndex : uint8_t {
    HEATER   = 0,
    COOLER   = 1,
    PH_UP    = 2,
    PH_DOWN  = 3,
    PUMP_IN  = 4,
    PUMP_OUT = 5,
    COUNT    = 6
};

// ----------------------------------------------------------------
// RELAY COMMAND — lệnh bật/tắt 6 relay
// true = yêu cầu BẬT relay, false = tắt
// ----------------------------------------------------------------
struct RelayCommand {
    bool heater   = false;
    bool cooler   = false;
    bool ph_up    = false;
    bool ph_down  = false;
    bool pump_in  = false;
    bool pump_out = false;

    // Helper: tắt tất cả
    void allOff() {
        heater = cooler = ph_up = ph_down = pump_in = pump_out = false;
    }

    // Truy cập theo RelayIndex
    bool& operator[](RelayIndex idx) {
        switch (idx) {
            case RelayIndex::HEATER:   return heater;
            case RelayIndex::COOLER:   return cooler;
            case RelayIndex::PH_UP:    return ph_up;
            case RelayIndex::PH_DOWN:  return ph_down;
            case RelayIndex::PUMP_IN:  return pump_in;
            case RelayIndex::PUMP_OUT: return pump_out;
            default:                   return heater; // fallback
        }
    }
};

// ----------------------------------------------------------------
// SAFETY EVENT — các sự kiện ghi log / gửi Firebase
// ※ Đã bỏ các event liên quan water level
// ----------------------------------------------------------------
enum class SafetyEvent : uint8_t {
    NONE                  = 0,
    THERMAL_CUTOFF        = 1,   // T >= thermal_cutoff → tắt tất cả
    EMERGENCY_COOL        = 2,   // T >= temp_emergency_cool → bật cooler
    HEATER_RUNTIME_LIMIT  = 3,   // Heater chạy quá lâu → cooldown
    HEATER_COOLDOWN       = 4,   // Đang trong cooldown
    SENSOR_UNRELIABLE     = 5,   // FALLBACK_DEFAULT → relay tắt
    SENSOR_STALE          = 6,   // fallback_count >= threshold → relay tắt
    MUTUAL_EXCLUSION      = 7,   // Xung đột relay → tắt cả cặp
    PH_PUMP_INTERVAL      = 8,   // pH pump bật quá sớm → block
    SHOCK_GUARD           = 9,   // Shock flag → tạm dừng điều khiển
};

// ----------------------------------------------------------------
// SAFETY LIMITS — runtime, Admin cập nhật qua Firebase (bước 17b)
// Có giá trị mặc định compile-time an toàn.
// ----------------------------------------------------------------
struct SafetyLimits {
    float    thermal_cutoff_c        = 42.0f;          // Tắt tất cả khi T >= giá trị này
    float    temp_emergency_cool_c   = 38.0f;          // Bật cooler khẩn cấp khi T >= giá trị này
    uint32_t heater_max_runtime_ms   = 10UL * 60000UL; // 10 phút
    uint32_t heater_cooldown_ms      = 5UL  * 60000UL; // 5 phút
    uint32_t ph_pump_max_pulse_ms    = 3000UL;         // 3 giây
    uint32_t ph_pump_min_interval_ms = 30000UL;        // 30 giây
    uint8_t  stale_sensor_threshold  = 12;              // Số chu kỳ fallback liên tiếp
    // ※ LOẠI BỎ: water_critical_low_cm
};

// Validator cho SafetyLimits — trả false nếu nguy hiểm
bool validateSafetyLimits(const SafetyLimits& lim);

// ----------------------------------------------------------------
// SAFETY CORE
// ----------------------------------------------------------------
class SafetyCore {
    public:
    SafetyCore();
    // Trong class SafetyCore public:
    void setBypass(bool bypass) { _bypass = bypass; }
    
    // Gọi trong setup(): pinMode relay pins, ghi tất cả OFF
    void begin();

    // Cập nhật SafetyLimits runtime (từ Firebase / Admin)
    // Validate trước khi áp dụng; trả false nếu bị reject
    bool setLimits(const SafetyLimits& lim);

    const SafetyLimits& getLimits() const { return _lim; }

    // Áp dụng 7 check an toàn tuần tự lên RelayCommand
    // Có thể tắt relay nếu vi phạm an toàn.
    // Trả về SafetyEvent đầu tiên bị kích hoạt (hoặc NONE)
    SafetyEvent apply(RelayCommand& cmd, const CleanReading& clean);

    // Gọi từ PhSessionManager trước khi bật pH pump.
    // Check ph_pump_min_interval — trả true nếu được phép, false nếu bị block.
    // Nếu được phép → cập nhật _lastPhPumpOnTime ngay.
    bool checkPhPumpAllowed();

    // Ghi RelayCommand vào GPIO vật lý
    // Relay active LOW: BẬT = digitalWrite LOW, TẮT = HIGH
    void writeRelays(const RelayCommand& cmd);

    // Lấy trạng thái relay hiện tại (sau lần writeRelays gần nhất)
    const RelayCommand& currentState() const { return _currentState; }

    // Lấy sự kiện safety gần nhất (để gửi Firebase)
    SafetyEvent lastEvent() const { return _lastEvent; }




private:
    // Trong private:
    bool _bypass = false;

    SafetyLimits  _lim;
    RelayCommand  _currentState;
    SafetyEvent   _lastEvent;

    // ---- Heater runtime tracking ----
    bool     _heaterOn;
    uint32_t _heaterOnTime;      // millis() khi heater bật
    bool     _heaterCooldown;
    uint32_t _heaterCooldownEnd; // millis() khi cooldown kết thúc

    // ---- pH pump timing ----
    uint32_t _lastPhPumpOnTime;  // millis() lần cuối pH pump bật

    // ---- Shock guard ----
    bool _shockHold;  // true = đang tạm dừng 1 chu kỳ vì shock

    // ---- 7 check methods ----
    SafetyEvent _checkSensorReliability(RelayCommand& cmd, const CleanReading& clean);
    SafetyEvent _checkStaleSensor      (RelayCommand& cmd, const CleanReading& clean);
    SafetyEvent _checkThermal          (RelayCommand& cmd, const CleanReading& clean);
    SafetyEvent _checkHeaterRuntime    (RelayCommand& cmd);
    SafetyEvent _checkMutualExclusion  (RelayCommand& cmd);
    SafetyEvent _checkPhPumpTiming     (RelayCommand& cmd);
    SafetyEvent _checkShockGuard       (RelayCommand& cmd, const CleanReading& clean);

    // GPIO pin cho từng relay (active LOW)
    static uint8_t _relayPin(RelayIndex idx);
};

// Global singleton
extern SafetyCore safetyCore;