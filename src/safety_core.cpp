#include "safety_core.h"
#include "system_constants.h"
#include "logger.h"
#include <Arduino.h>

// ================================================================
// safety_core.cpp
// Intelligent Aquarium v4.0
//
// 7 check an toàn tuần tự. Mỗi check có thể tắt relay nhưng
// không bao giờ bật relay — chỉ controller mới bật.
// ================================================================

// Global singleton
SafetyCore safetyCore;

const char* safetyEventToString(SafetyEvent evt) {
    switch (evt) {
        case SafetyEvent::NONE:                 return "NONE";
        case SafetyEvent::THERMAL_CUTOFF:       return "THERMAL_CUTOFF";
        case SafetyEvent::EMERGENCY_COOL:       return "EMERGENCY_COOL";
        case SafetyEvent::HEATER_RUNTIME_LIMIT: return "HEATER_RUNTIME_LIMIT";
        case SafetyEvent::HEATER_COOLDOWN:      return "HEATER_COOLDOWN";
        case SafetyEvent::SENSOR_UNRELIABLE:    return "SENSOR_UNRELIABLE";
        case SafetyEvent::SENSOR_STALE:         return "SENSOR_STALE";
        case SafetyEvent::MUTUAL_EXCLUSION:     return "MUTUAL_EXCLUSION";
        case SafetyEvent::PH_PUMP_INTERVAL:     return "PH_PUMP_INTERVAL";
        case SafetyEvent::SHOCK_GUARD:          return "SHOCK_GUARD";
        default:                                return "UNKNOWN";
    }
}

// ----------------------------------------------------------------
// SafetyLimits validator
// ----------------------------------------------------------------
bool validateSafetyLimits(const SafetyLimits& lim) {
    if (lim.thermal_cutoff_c > 50.0f || lim.thermal_cutoff_c < 30.0f) {
        LOG_ERROR("SAFETY", "thermal_cutoff out of safe range [30,50]: %.1f", lim.thermal_cutoff_c);
        return false;
    }
    if (lim.temp_emergency_cool_c >= lim.thermal_cutoff_c) {
        LOG_ERROR("SAFETY", "temp_emergency_cool (%.1f) >= thermal_cutoff (%.1f)",
                  lim.temp_emergency_cool_c, lim.thermal_cutoff_c);
        return false;
    }
    if (lim.temp_emergency_cool_c < 25.0f) {
        LOG_ERROR("SAFETY", "temp_emergency_cool too low: %.1f", lim.temp_emergency_cool_c);
        return false;
    }
    if (lim.heater_max_runtime_ms < 60000UL || lim.heater_max_runtime_ms > 3600000UL) {
        LOG_ERROR("SAFETY", "heater_max_runtime out of [1min, 60min]");
        return false;
    }
    if (lim.ph_pump_min_interval_ms < lim.ph_pump_max_pulse_ms * 2) {
        LOG_ERROR("SAFETY", "ph_pump_min_interval must be >= 2x max_pulse");
        return false;
    }
    if (lim.stale_sensor_threshold < 3 || lim.stale_sensor_threshold > 20) {
        LOG_ERROR("SAFETY", "stale_sensor_threshold out of [3,20]: %d", lim.stale_sensor_threshold);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
// Pin mapping: RelayIndex → GPIO
// ----------------------------------------------------------------
uint8_t SafetyCore::_relayPin(RelayIndex idx) {
    switch (idx) {
        case RelayIndex::HEATER:   return PIN_RELAY_HEATER;
        case RelayIndex::COOLER:   return PIN_RELAY_COOLER;
        case RelayIndex::PH_UP:    return PIN_RELAY_PH_UP;
        case RelayIndex::PH_DOWN:  return PIN_RELAY_PH_DOWN;
        case RelayIndex::PUMP_IN:  return PIN_RELAY_PUMP_IN;
        case RelayIndex::PUMP_OUT: return PIN_RELAY_PUMP_OUT;
        default:                   return 255;
    }
}

// ================================================================
// CONSTRUCTOR
// ================================================================
SafetyCore::SafetyCore()
    : _lastEvent(SafetyEvent::NONE),
      _heaterOn(false), _heaterOnTime(0),
      _heaterCooldown(false), _heaterCooldownEnd(0),
      _lastPhPumpOnTime(0),
      _shockHold(false)
{}

// ================================================================
// BEGIN
// ================================================================
void SafetyCore::begin() {
    // Khởi tạo tất cả relay pins OUTPUT, ghi HIGH (active LOW = OFF)
    const RelayIndex relays[] = {
        RelayIndex::HEATER, RelayIndex::COOLER,
        RelayIndex::PH_UP,  RelayIndex::PH_DOWN,
        RelayIndex::PUMP_IN, RelayIndex::PUMP_OUT
    };
    for (auto r : relays) {
        uint8_t pin = _relayPin(r);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);  // Tắt (active LOW)
    }
    LOG_INFO("SAFETY", "SafetyCore begin — all relays OFF");
}

// ================================================================
// SET LIMITS (runtime, từ Firebase)
// ================================================================
bool SafetyCore::setLimits(const SafetyLimits& lim) {
    if (!validateSafetyLimits(lim)) return false;
    _lim = lim;
    LOG_INFO("SAFETY", "SafetyLimits updated: cutoff=%.1f emgCool=%.1f",
             lim.thermal_cutoff_c, lim.temp_emergency_cool_c);
    return true;
}

// ================================================================
// APPLY — 7 check tuần tự
// ================================================================
SafetyEvent SafetyCore::apply(RelayCommand& cmd, const CleanReading& clean) {
    // Nếu bypass mode → không check, giữ nguyên cmd, trả NONE
    if (_bypass) {
        _lastEvent = SafetyEvent::NONE;
        return SafetyEvent::NONE;
    }
    
    SafetyEvent triggered = SafetyEvent::NONE;

    // Chạy 7 check theo thứ tự. Mỗi check có thể tắt relay.
    // Dừng sớm khi gặp event nghiêm trọng (THERMAL_CUTOFF tắt tất cả).

    triggered = _checkSensorReliability(cmd, clean);
    if (triggered != SafetyEvent::NONE) goto done;

    triggered = _checkStaleSensor(cmd, clean);
    // Không dừng sớm — stale chỉ tắt relay liên quan, tiếp tục check khác

    {
        SafetyEvent t = _checkThermal(cmd, clean);
        if (t == SafetyEvent::THERMAL_CUTOFF) { triggered = t; goto done; }
        if (t != SafetyEvent::NONE && triggered == SafetyEvent::NONE) triggered = t;
    }

    {
        SafetyEvent t = _checkHeaterRuntime(cmd);
        if (t != SafetyEvent::NONE && triggered == SafetyEvent::NONE) triggered = t;
    }

    {
        SafetyEvent t = _checkMutualExclusion(cmd);
        if (t != SafetyEvent::NONE && triggered == SafetyEvent::NONE) triggered = t;
    }

    {
        SafetyEvent t = _checkPhPumpTiming(cmd);
        if (t != SafetyEvent::NONE && triggered == SafetyEvent::NONE) triggered = t;
    }

    {
        SafetyEvent t = _checkShockGuard(cmd, clean);
        if (t != SafetyEvent::NONE && triggered == SafetyEvent::NONE) triggered = t;
    }

done:
    _lastEvent = triggered;
    return triggered;
}

// ================================================================
// CHECK 1 — SENSOR RELIABILITY
// Nếu bất kỳ field nào là FALLBACK_DEFAULT → tắt relay điều khiển
// bởi field đó. FALLBACK_DEFAULT = chưa có bất kỳ giá trị hợp lệ nào.
// ================================================================
SafetyEvent SafetyCore::_checkSensorReliability(RelayCommand& cmd, const CleanReading& clean) {
    bool triggered = false;

    if (clean.source_temperature == DataSource::FALLBACK_DEFAULT) {
        if (cmd.heater || cmd.cooler) {
            cmd.heater = cmd.cooler = false;
            LOG_ERROR("SAFETY", "Temp FALLBACK_DEFAULT → heater+cooler OFF");
            triggered = true;
        }
    }

    // pH không có trong CleanReading — pH pump kiểm soát bởi PhSessionManager
    // Không cần check source_ph ở đây

    // TDS không có relay → chỉ log
    if (clean.source_tds == DataSource::FALLBACK_DEFAULT) {
        LOG_WARNING("SAFETY", "TDS FALLBACK_DEFAULT (no relay to disable)");
    }

    return triggered ? SafetyEvent::SENSOR_UNRELIABLE : SafetyEvent::NONE;
}

// ================================================================
// CHECK 2 — STALE SENSOR
// fallback_count >= stale_sensor_threshold → sensor coi như hư
// ================================================================
SafetyEvent SafetyCore::_checkStaleSensor(RelayCommand& cmd, const CleanReading& clean) {
    bool triggered = false;
    uint8_t threshold = _lim.stale_sensor_threshold;

    if (clean.fallback_count_temp >= threshold) {
        if (cmd.heater || cmd.cooler) {
            cmd.heater = cmd.cooler = false;
            LOG_WARNING("SAFETY", "Temp stale (%d >= %d) → heater+cooler OFF",
                        clean.fallback_count_temp, threshold);
            triggered = true;
        }
    }

    // pH pump không check stale qua pipeline — kiểm soát bởi PhSessionManager

    // TDS stale → chỉ log
    if (clean.fallback_count_tds >= threshold) {
        LOG_WARNING("SAFETY", "TDS stale (%d >= %d) — no relay action",
                    clean.fallback_count_tds, threshold);
    }

    return triggered ? SafetyEvent::SENSOR_STALE : SafetyEvent::NONE;
}

// ================================================================
// CHECK 3 — THERMAL
// >= thermal_cutoff_c : TẮT TẤT CẢ (emergency)
// >= temp_emergency_cool_c : bật cooler, tắt heater
// ================================================================
SafetyEvent SafetyCore::_checkThermal(RelayCommand& cmd, const CleanReading& clean) {
    // Chỉ check khi nhiệt độ hợp lệ
    if (clean.source_temperature == DataSource::FALLBACK_DEFAULT) {
        return SafetyEvent::NONE;
    }

    float t = clean.temperature;

    if (t >= _lim.thermal_cutoff_c) {
        cmd.allOff();
        LOG_ERROR("SAFETY", "THERMAL CUTOFF! T=%.2f >= %.1f → ALL RELAYS OFF",
                  t, _lim.thermal_cutoff_c);
        return SafetyEvent::THERMAL_CUTOFF;
    }

    if (t >= _lim.temp_emergency_cool_c) {
        cmd.heater = false;
        cmd.cooler = true;
        LOG_WARNING("SAFETY", "Emergency cool: T=%.2f >= %.1f → cooler ON, heater OFF",
                    t, _lim.temp_emergency_cool_c);
        return SafetyEvent::EMERGENCY_COOL;
    }

    return SafetyEvent::NONE;
}

// ================================================================
// CHECK 4 — HEATER RUNTIME
// Heater chạy liên tục quá heater_max_runtime_ms → buộc cooldown
// Sau cooldown_ms mới cho phép bật lại
// ================================================================
SafetyEvent SafetyCore::_checkHeaterRuntime(RelayCommand& cmd) {
    uint32_t now = millis();

    // Đang trong cooldown?
    if (_heaterCooldown) {
        if (now >= _heaterCooldownEnd) {
            _heaterCooldown = false;
            LOG_INFO("SAFETY", "Heater cooldown ended");
        } else {
            if (cmd.heater) {
                cmd.heater = false;
                LOG_WARNING("SAFETY", "Heater blocked: cooldown %lus remaining",
                            (unsigned long)((_heaterCooldownEnd - now) / 1000));
            }
            return SafetyEvent::HEATER_COOLDOWN;
        }
    }

    // Theo dõi thời gian heater ON
    if (cmd.heater) {
        if (!_heaterOn) {
            // Vừa bật
            _heaterOn     = true;
            _heaterOnTime = now;
        } else {
            // Đang bật — kiểm tra thời gian
            uint32_t runtime = now - _heaterOnTime;
            if (runtime >= _lim.heater_max_runtime_ms) {
                cmd.heater        = false;
                _heaterOn         = false;
                _heaterCooldown   = true;
                _heaterCooldownEnd = now + _lim.heater_cooldown_ms;
                LOG_WARNING("SAFETY", "Heater max runtime reached (%lus) → cooldown %lus",
                            (unsigned long)(_lim.heater_max_runtime_ms / 1000),
                            (unsigned long)(_lim.heater_cooldown_ms / 1000));
                return SafetyEvent::HEATER_RUNTIME_LIMIT;
            }
        }
    } else {
        // Heater tắt → reset tracker
        _heaterOn = false;
    }

    return SafetyEvent::NONE;
}

// ================================================================
// CHECK 5 — MUTUAL EXCLUSION
// Không bao giờ bật 2 relay xung đột cùng lúc:
//   heater + cooler
//   ph_up  + ph_down
//   pump_in + pump_out
// Khi xung đột → tắt cả hai (an toàn nhất)
// ================================================================
SafetyEvent SafetyCore::_checkMutualExclusion(RelayCommand& cmd) {
    bool triggered = false;

    if (cmd.heater && cmd.cooler) {
        cmd.heater = cmd.cooler = false;
        LOG_ERROR("SAFETY", "Mutual exclusion: heater+cooler both ON → both OFF");
        triggered = true;
    }

    if (cmd.ph_up && cmd.ph_down) {
        cmd.ph_up = cmd.ph_down = false;
        LOG_ERROR("SAFETY", "Mutual exclusion: ph_up+ph_down both ON → both OFF");
        triggered = true;
    }

    if (cmd.pump_in && cmd.pump_out) {
        cmd.pump_in = cmd.pump_out = false;
        LOG_ERROR("SAFETY", "Mutual exclusion: pump_in+pump_out both ON → both OFF");
        triggered = true;
    }

    return triggered ? SafetyEvent::MUTUAL_EXCLUSION : SafetyEvent::NONE;
}

// ================================================================
// CHECK 6 — PH PUMP TIMING
// Đảm bảo khoảng cách tối thiểu giữa các lần bật pH pump.
// Safety core KHÔNG kiểm soát pulse duration (đó là việc của
// pid_controller + tickPhPulse trong main.cpp).
// ================================================================
SafetyEvent SafetyCore::_checkPhPumpTiming(RelayCommand& cmd) {
    bool wantsPhPump = cmd.ph_up || cmd.ph_down;
    if (!wantsPhPump) return SafetyEvent::NONE;

    uint32_t now = millis();
    uint32_t elapsed = now - _lastPhPumpOnTime;

    if (_lastPhPumpOnTime > 0 && elapsed < _lim.ph_pump_min_interval_ms) {
        cmd.ph_up = cmd.ph_down = false;
        LOG_WARNING("SAFETY", "pH pump blocked: interval %lums < min %lums",
                    (unsigned long)elapsed,
                    (unsigned long)_lim.ph_pump_min_interval_ms);
        return SafetyEvent::PH_PUMP_INTERVAL;
    }

    // Cho phép → ghi nhận thời điểm
    _lastPhPumpOnTime = now;
    return SafetyEvent::NONE;
}

// ================================================================
// checkPhPumpAllowed — gọi từ PhSessionManager trước _startPulse()
// Tái dùng logic _checkPhPumpTiming nhưng không cần RelayCommand.
// Trả true → được phép, cập nhật _lastPhPumpOnTime.
// Trả false → bị block (interval chưa đủ).
// ================================================================
bool SafetyCore::checkPhPumpAllowed() {
    uint32_t now     = millis();
    uint32_t elapsed = now - _lastPhPumpOnTime;

    if (_lastPhPumpOnTime > 0 && elapsed < _lim.ph_pump_min_interval_ms) {
        LOG_WARNING("SAFETY", "checkPhPumpAllowed: blocked — interval %lums < min %lums",
                    (unsigned long)elapsed,
                    (unsigned long)_lim.ph_pump_min_interval_ms);
        return false;
    }

    // Cho phép → ghi nhận thời điểm ngay
    _lastPhPumpOnTime = now;
    LOG_DEBUG("SAFETY", "checkPhPumpAllowed: OK (elapsed=%lums)", (unsigned long)elapsed);
    return true;
}


// Nếu có shock flag → tạm dừng điều khiển pH pump 1 chu kỳ.
// KHÔNG tắt heater/cooler khi đang chạy ổn định — shock nhiệt độ
// có thể do nước thay nước, sò, hoặc cooler/heater đang hoạt động
// bình thường. Tắt cooler/heater giữa chừng gây chattering.
// ================================================================
SafetyEvent SafetyCore::_checkShockGuard(RelayCommand& cmd, const CleanReading& clean) {
    bool hasShock = clean.has_shock();

    if (_shockHold) {
        // Chu kỳ trước có shock → chỉ tạm dừng pH pump, không đụng heater/cooler
        cmd.ph_up   = false;
        cmd.ph_down = false;
        _shockHold  = false;
        LOG_WARNING("SAFETY", "ShockGuard: holding pH pump 1 cycle");
        return SafetyEvent::SHOCK_GUARD;
    }

    if (hasShock) {
        _shockHold = true;
        LOG_WARNING("SAFETY", "Shock detected (T=%d) → will hold pH pump next cycle",
                    clean.shock_temperature);
    }

    return SafetyEvent::NONE;
}

// ================================================================
// WRITE RELAYS — ghi RelayCommand ra GPIO vật lý
// Relay active LOW: ON = LOW, OFF = HIGH
// ================================================================
void SafetyCore::writeRelays(const RelayCommand& cmd) {
    // Chỉ ghi khi state thay đổi để tránh bouncing
    auto write = [](uint8_t pin, bool on, bool wasOn) {
        if (on != wasOn) {
            digitalWrite(pin, on ? LOW : HIGH);
        }
    };

    write(PIN_RELAY_HEATER,   cmd.heater,   _currentState.heater);
    write(PIN_RELAY_COOLER,   cmd.cooler,   _currentState.cooler);
    write(PIN_RELAY_PH_UP,    cmd.ph_up,    _currentState.ph_up);
    write(PIN_RELAY_PH_DOWN,  cmd.ph_down,  _currentState.ph_down);
    write(PIN_RELAY_PUMP_IN,  cmd.pump_in,  _currentState.pump_in);
    write(PIN_RELAY_PUMP_OUT, cmd.pump_out, _currentState.pump_out);

    // Log chỉ khi state thay đổi
    if (cmd.heater   != _currentState.heater   ||
        cmd.cooler   != _currentState.cooler   ||
        cmd.ph_up    != _currentState.ph_up    ||
        cmd.ph_down  != _currentState.ph_down  ||
        cmd.pump_in  != _currentState.pump_in  ||
        cmd.pump_out != _currentState.pump_out) {

        LOG_DEBUG("RELAY", "H=%d C=%d pHU=%d pHD=%d pIn=%d pOut=%d",
                  cmd.heater, cmd.cooler, cmd.ph_up, cmd.ph_down,
                  cmd.pump_in, cmd.pump_out);
    }

    _currentState = cmd;
}