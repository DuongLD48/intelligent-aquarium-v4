#include "control_config.h"

// ================================================================
// control_config.cpp
// Intelligent Aquarium v4.0
// ================================================================

// ----------------------------------------------------------------
ConfigError ConfigValidator::validate(const ControlConfig& cfg) {
    // --- Temperature ---
    if (cfg.temp_min < 10.0f)                          return ConfigError::TEMP_MIN_TOO_LOW;
    if (cfg.temp_max > 40.0f)                          return ConfigError::TEMP_MAX_TOO_HIGH;
    if (cfg.temp_min >= cfg.temp_max)                  return ConfigError::TEMP_MIN_GE_MAX;
    if ((cfg.temp_max - cfg.temp_min) < 2.0f)          return ConfigError::TEMP_RANGE_TOO_NARROW;

    // --- pH ---
    if (cfg.ph_min < 4.0f)                             return ConfigError::PH_MIN_TOO_LOW;
    if (cfg.ph_max > 10.0f)                            return ConfigError::PH_MAX_TOO_HIGH;
    if (cfg.ph_min >= cfg.ph_max)                      return ConfigError::PH_MIN_GE_MAX;
    if ((cfg.ph_max - cfg.ph_min) < 0.3f)              return ConfigError::PH_RANGE_TOO_NARROW;

    // --- TDS ---
    if (cfg.tds_target < 10.0f)                        return ConfigError::TDS_TARGET_TOO_LOW;
    if (cfg.tds_target > 2000.0f)                      return ConfigError::TDS_TARGET_TOO_HIGH;
    if (cfg.tds_tolerance <= 0.0f)                     return ConfigError::TDS_TOLERANCE_INVALID;

    // --- PID ---
    if (cfg.pid_kp < 0.0f)                             return ConfigError::PID_KP_NEGATIVE;
    if (cfg.pid_ki < 0.0f)                             return ConfigError::PID_KI_NEGATIVE;
    if (cfg.pid_kd < 0.0f)                             return ConfigError::PID_KD_NEGATIVE;

    return ConfigError::OK;
}

// ----------------------------------------------------------------
const char* ConfigValidator::errorString(ConfigError err) {
    switch (err) {
        case ConfigError::OK:                   return "OK";
        case ConfigError::TEMP_MIN_TOO_LOW:     return "temp_min < 10";
        case ConfigError::TEMP_MAX_TOO_HIGH:    return "temp_max > 40";
        case ConfigError::TEMP_RANGE_TOO_NARROW:return "temp range < 2 deg";
        case ConfigError::TEMP_MIN_GE_MAX:      return "temp_min >= temp_max";
        case ConfigError::PH_MIN_TOO_LOW:       return "ph_min < 4.0";
        case ConfigError::PH_MAX_TOO_HIGH:      return "ph_max > 10.0";
        case ConfigError::PH_RANGE_TOO_NARROW:  return "ph range < 0.3";
        case ConfigError::PH_MIN_GE_MAX:        return "ph_min >= ph_max";
        case ConfigError::TDS_TARGET_TOO_LOW:   return "tds_target < 10";
        case ConfigError::TDS_TARGET_TOO_HIGH:  return "tds_target > 2000";
        case ConfigError::TDS_TOLERANCE_INVALID:return "tds_tolerance <= 0";
        case ConfigError::PID_KP_NEGATIVE:      return "pid_kp < 0";
        case ConfigError::PID_KI_NEGATIVE:      return "pid_ki < 0";
        case ConfigError::PID_KD_NEGATIVE:      return "pid_kd < 0";
        default:                                return "unknown error";
    }
}
