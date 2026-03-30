#include "config_manager.h"
#include "logger.h"
#include "hysteresis_controller.h"
#include "data_pipeline.h"
#include "water_change_manager.h"
#include "sensor_manager.h"
#include "ph_dose_controller.h"
#include "ph_session_manager.h"
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ================================================================
// config_manager.cpp
// Intelligent Aquarium v4.1
// Thay đổi: bỏ pidCtrl, thêm PhDoseConfig load/save/parse/apply
// ================================================================

extern HysteresisController hysteresisCtrl;
extern DataPipeline         dataPipeline;
extern WaterChangeManager   waterChangeManager;

ConfigManager configManager;

// ================================================================
// JSON HELPERS — minimal parser, không dùng ArduinoJson
// ================================================================
bool ConfigManager::_findJsonValue(const char* json, const char* key,
                                    char* valueBuf, size_t bufSize) {
    if (!json || !key || !valueBuf) return false;
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);
    const char* pos = strstr(json, searchKey);
    if (!pos) return false;
    pos += strlen(searchKey);
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    size_t i = 0;
    if (*pos == '"') {
        pos++;
        while (*pos && *pos != '"' && i < bufSize - 1) valueBuf[i++] = *pos++;
    } else {
        while (*pos && *pos != ',' && *pos != '}' && *pos != ' ' && i < bufSize - 1)
            valueBuf[i++] = *pos++;
    }
    valueBuf[i] = '\0';
    return (i > 0);
}

bool ConfigManager::_parseFloat(const char* json, const char* key, float& out) {
    char buf[32];
    if (!_findJsonValue(json, key, buf, sizeof(buf))) return false;
    out = atof(buf); return true;
}
bool ConfigManager::_parseInt(const char* json, const char* key, int& out) {
    char buf[32];
    if (!_findJsonValue(json, key, buf, sizeof(buf))) return false;
    out = atoi(buf); return true;
}
bool ConfigManager::_parseBool(const char* json, const char* key, bool& out) {
    char buf[8];
    if (!_findJsonValue(json, key, buf, sizeof(buf))) return false;
    out = (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0); return true;
}

// ================================================================
// CONSTRUCTOR
// ================================================================
ConfigManager::ConfigManager() : _serialPos(0) {
    memset(_serialBuf, 0, sizeof(_serialBuf));
}

// ================================================================
// BEGIN
// ================================================================
void ConfigManager::begin() {
    bool ctrlOk  = loadControlConfig(_ctrl);
    bool pipeOk  = loadPipelineConfig(_pipe);
    bool waterOk = loadWaterSchedule(_water);
    bool calibOk = loadCalibration(_calib);
    bool doseOk  = loadPhDoseConfig(_dose);

    LOG_INFO("CFG", "begin: ctrl=%s pipe=%s water=%s calib=%s dose=%s",
             ctrlOk  ? "NVS" : "default",
             pipeOk  ? "NVS" : "default",
             waterOk ? "NVS" : "default",
             calibOk ? "NVS" : "default",
             doseOk  ? "NVS" : "default");
}

// ================================================================
// NVS — ControlConfig
// ================================================================
bool ConfigManager::loadControlConfig(ControlConfig& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_CTRL, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }
    out.temp_min      = prefs.getFloat("temp_min",  out.temp_min);
    out.temp_max      = prefs.getFloat("temp_max",  out.temp_max);
    out.ph_min        = prefs.getFloat("ph_min",    out.ph_min);
    out.ph_max        = prefs.getFloat("ph_max",    out.ph_max);
    out.tds_target    = prefs.getFloat("tds_tgt",   out.tds_target);
    out.tds_tolerance = prefs.getFloat("tds_tol",   out.tds_tolerance);
    // PID fields kept for backward NVS compat but ignored at runtime
    prefs.end();
    LOG_INFO("CFG", "ControlConfig loaded from NVS");
    return true;
}

bool ConfigManager::saveControlConfig(const ControlConfig& cfg) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_CTRL, false)) return false;
    prefs.putFloat("temp_min", cfg.temp_min);
    prefs.putFloat("temp_max", cfg.temp_max);
    prefs.putFloat("ph_min",   cfg.ph_min);
    prefs.putFloat("ph_max",   cfg.ph_max);
    prefs.putFloat("tds_tgt",  cfg.tds_target);
    prefs.putFloat("tds_tol",  cfg.tds_tolerance);
    prefs.putBool("saved", true);
    prefs.end();
    return true;
}

// ================================================================
// NVS — PipelineConfig
// ================================================================
bool ConfigManager::loadPipelineConfig(PipelineConfig& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_PIPELINE, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }
    out.temp_min        = prefs.getFloat("t_min",  out.temp_min);
    out.temp_max        = prefs.getFloat("t_max",  out.temp_max);
    out.tds_min         = prefs.getFloat("s_min",  out.tds_min);
    out.tds_max         = prefs.getFloat("s_max",  out.tds_max);
    out.mad_window_size = (size_t)prefs.getUInt("mad_win",  (uint32_t)out.mad_window_size);
    out.mad_min_samples = (size_t)prefs.getUInt("mad_mns",  (uint32_t)out.mad_min_samples);
    out.mad_threshold   = prefs.getFloat("mad_thr",  out.mad_threshold);
    out.mad_floor_temp  = prefs.getFloat("mad_ft",   out.mad_floor_temp);
    out.mad_floor_tds   = prefs.getFloat("mad_fs",   out.mad_floor_tds);
    out.shock_temp_delta= prefs.getFloat("shk_t",    out.shock_temp_delta);
    prefs.end();
    LOG_INFO("CFG", "PipelineConfig loaded from NVS");
    return true;
}

bool ConfigManager::savePipelineConfig(const PipelineConfig& cfg) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_PIPELINE, false)) return false;
    prefs.putFloat("t_min",  cfg.temp_min);
    prefs.putFloat("t_max",  cfg.temp_max);
    prefs.putFloat("s_min",  cfg.tds_min);
    prefs.putFloat("s_max",  cfg.tds_max);
    prefs.putUInt("mad_win", (uint32_t)cfg.mad_window_size);
    prefs.putUInt("mad_mns", (uint32_t)cfg.mad_min_samples);
    prefs.putFloat("mad_thr",  cfg.mad_threshold);
    prefs.putFloat("mad_ft",   cfg.mad_floor_temp);
    prefs.putFloat("mad_fs",   cfg.mad_floor_tds);
    prefs.putFloat("shk_t",    cfg.shock_temp_delta);
    prefs.putBool("saved", true);
    prefs.end();
    return true;
}

// ================================================================
// NVS — WaterChangeSchedule
// ================================================================
bool ConfigManager::loadWaterSchedule(WaterChangeSchedule& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_WATER, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }
    out.enabled          = prefs.getBool("enabled",   out.enabled);
    out.hour             = (uint8_t)prefs.getUInt("hour",   out.hour);
    out.minute           = (uint8_t)prefs.getUInt("minute", out.minute);
    out.pump_out_sec     = (uint16_t)prefs.getUInt("p_out",  out.pump_out_sec);
    out.pump_in_sec      = (uint16_t)prefs.getUInt("p_in",   out.pump_in_sec);
    out.pump_min_sec     = (uint16_t)prefs.getUInt("p_min",  out.pump_min_sec);
    out.pump_out_max_sec = (uint16_t)prefs.getUInt("p_omax", out.pump_out_max_sec);
    out.pump_in_max_sec  = (uint16_t)prefs.getUInt("p_imax", out.pump_in_max_sec);
    out.last_run_day     = prefs.getUInt("lr_day", out.last_run_day);
    out.last_run_ts      = prefs.getUInt("lr_ts",  out.last_run_ts);
    prefs.end();
    return true;
}

bool ConfigManager::saveWaterSchedule(const WaterChangeSchedule& sched) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_WATER, false)) return false;
    prefs.putBool("enabled",  sched.enabled);
    prefs.putUInt("hour",     sched.hour);
    prefs.putUInt("minute",   sched.minute);
    prefs.putUInt("p_out",    sched.pump_out_sec);
    prefs.putUInt("p_in",     sched.pump_in_sec);
    prefs.putUInt("p_min",    sched.pump_min_sec);
    prefs.putUInt("p_omax",   sched.pump_out_max_sec);
    prefs.putUInt("p_imax",   sched.pump_in_max_sec);
    prefs.putUInt("lr_day",   sched.last_run_day);
    prefs.putUInt("lr_ts",    sched.last_run_ts);
    prefs.putBool("saved", true);
    prefs.end();
    return true;
}

// ================================================================
// NVS — SensorCalibration
// ================================================================
bool ConfigManager::loadCalibration(SensorCalibration& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_CALIB, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }
    out.ph_slope   = prefs.getFloat("ph_slope",   out.ph_slope);
    out.ph_offset  = prefs.getFloat("ph_offset",  out.ph_offset);
    out.tds_factor = prefs.getFloat("tds_factor", out.tds_factor);
    prefs.end();
    if (!out.isValid()) { out = SensorCalibration{}; return false; }
    return true;
}

bool ConfigManager::saveCalibration(const SensorCalibration& calib) {
    if (!calib.isValid()) return false;
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_CALIB, false)) return false;
    prefs.putFloat("ph_slope",   calib.ph_slope);
    prefs.putFloat("ph_offset",  calib.ph_offset);
    prefs.putFloat("tds_factor", calib.tds_factor);
    prefs.putBool("saved", true);
    prefs.end();
    return true;
}

// ================================================================
// NVS — PhDoseConfig  (MỚI)
// ================================================================
bool ConfigManager::loadPhDoseConfig(PhDoseConfig& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_DOSE, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }

    out.base_pulse_ms       = prefs.getUInt("base_ms",   out.base_pulse_ms);
    out.pulse_per_unit      = prefs.getUInt("per_unit",  out.pulse_per_unit);
    out.max_pulse_ms        = prefs.getUInt("max_ms",    out.max_pulse_ms);
    // Lưu dưới dạng giây để tránh overflow uint32 NVS
    out.measure_interval_ms = (uint32_t)prefs.getUInt("iv_s",  (uint32_t)(out.measure_interval_ms / 1000)) * 1000UL;
    out.session_duration_ms = (uint32_t)prefs.getUInt("ses_s", (uint32_t)(out.session_duration_ms / 1000)) * 1000UL;
    out.warmup_ms           = (uint32_t)prefs.getUInt("wu_s",  (uint32_t)(out.warmup_ms           / 1000)) * 1000UL;
    out.noise_threshold     = prefs.getFloat("noise_thr", out.noise_threshold);
    out.shock_threshold     = prefs.getFloat("shock_thr", out.shock_threshold);
    prefs.end();

    if (!out.isValid()) {
        LOG_WARNING("CFG", "loadPhDoseConfig: NVS data invalid → default");
        out = PhDoseConfig{};
        return false;
    }
    LOG_INFO("CFG", "PhDoseConfig loaded: base=%lu per_unit=%lu interval=%lus",
             (unsigned long)out.base_pulse_ms,
             (unsigned long)out.pulse_per_unit,
             (unsigned long)(out.measure_interval_ms / 1000));
    return true;
}

bool ConfigManager::savePhDoseConfig(const PhDoseConfig& cfg) {
    if (!cfg.isValid()) return false;
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_DOSE, false)) return false;
    prefs.putUInt("base_ms",  cfg.base_pulse_ms);
    prefs.putUInt("per_unit", cfg.pulse_per_unit);
    prefs.putUInt("max_ms",   cfg.max_pulse_ms);
    prefs.putUInt("iv_s",  (uint32_t)(cfg.measure_interval_ms / 1000));
    prefs.putUInt("ses_s", (uint32_t)(cfg.session_duration_ms / 1000));
    prefs.putUInt("wu_s",  (uint32_t)(cfg.warmup_ms           / 1000));
    prefs.putFloat("noise_thr", cfg.noise_threshold);
    prefs.putFloat("shock_thr", cfg.shock_threshold);
    prefs.putBool("saved", true);
    prefs.end();
    return true;
}

// ================================================================
// JSON PARSERS
// ================================================================
bool ConfigManager::parseControlConfigJson(const char* json, ControlConfig& out) {
    ControlConfig tmp = out;
    _parseFloat(json, "temp_min",      tmp.temp_min);
    _parseFloat(json, "temp_max",      tmp.temp_max);
    _parseFloat(json, "ph_min",        tmp.ph_min);
    _parseFloat(json, "ph_max",        tmp.ph_max);
    _parseFloat(json, "tds_target",    tmp.tds_target);
    _parseFloat(json, "tds_tolerance", tmp.tds_tolerance);
    // PID fields parsed for compat but ignored
    ConfigError err = ConfigValidator::validate(tmp);
    if (err != ConfigError::OK) {
        LOG_ERROR("CFG", "parseControlConfigJson invalid: %s", ConfigValidator::errorString(err));
        return false;
    }
    out = tmp;
    return true;
}

bool ConfigManager::parsePipelineConfigJson(const char* json, PipelineConfig& out) {
    PipelineConfig tmp = out;
    _parseFloat(json, "temp_range_min",   tmp.temp_min);
    _parseFloat(json, "temp_range_max",   tmp.temp_max);
    _parseFloat(json, "tds_range_min",    tmp.tds_min);
    _parseFloat(json, "tds_range_max",    tmp.tds_max);
    _parseFloat(json, "mad_threshold",    tmp.mad_threshold);
    _parseFloat(json, "mad_floor_temp",   tmp.mad_floor_temp);
    _parseFloat(json, "mad_floor_tds",    tmp.mad_floor_tds);
    _parseFloat(json, "shock_temp_delta", tmp.shock_temp_delta);
    int iVal;
    if (_parseInt(json, "mad_window_size", iVal) && iVal >= 5 && iVal <= 30)
        tmp.mad_window_size = (size_t)iVal;
    if (_parseInt(json, "mad_min_samples", iVal) && iVal >= 1 && iVal <= (int)tmp.mad_window_size)
        tmp.mad_min_samples = (size_t)iVal;
    if (tmp.temp_min >= tmp.temp_max ||
        tmp.tds_min  >= tmp.tds_max  || tmp.mad_threshold <= 0.0f) {
        LOG_ERROR("CFG", "parsePipelineConfigJson: invalid range");
        return false;
    }
    out = tmp;
    return true;
}

bool ConfigManager::parseWaterScheduleJson(const char* json, WaterChangeSchedule& out) {
    WaterChangeSchedule tmp = out;
    bool bVal; int iVal;
    if (_parseBool(json, "enabled", bVal)) tmp.enabled = bVal;
    if (_parseInt(json, "hour",   iVal) && iVal >= 0 && iVal <= 23) tmp.hour   = (uint8_t)iVal;
    if (_parseInt(json, "minute", iVal) && iVal >= 0 && iVal <= 59) tmp.minute = (uint8_t)iVal;
    if (_parseInt(json, "pump_min_sec",     iVal) && iVal >= WATER_CHANGE_MIN_PUMP_SEC)
        tmp.pump_min_sec = (uint16_t)iVal;
    if (_parseInt(json, "pump_out_max_sec", iVal) && iVal > tmp.pump_min_sec && iVal <= WATER_CHANGE_MAX_PUMP_OUT_SEC)
        tmp.pump_out_max_sec = (uint16_t)iVal;
    if (_parseInt(json, "pump_in_max_sec",  iVal) && iVal > tmp.pump_min_sec && iVal <= WATER_CHANGE_MAX_PUMP_IN_SEC)
        tmp.pump_in_max_sec = (uint16_t)iVal;
    if (_parseInt(json, "pump_out_sec", iVal) && iVal >= tmp.pump_min_sec && iVal <= tmp.pump_out_max_sec)
        tmp.pump_out_sec = (uint16_t)iVal;
    if (_parseInt(json, "pump_in_sec",  iVal) && iVal >= tmp.pump_min_sec && iVal <= tmp.pump_in_max_sec)
        tmp.pump_in_sec = (uint16_t)iVal;
    out = tmp;
    return true;
}

bool ConfigManager::parseCalibrationJson(const char* json, SensorCalibration& out) {
    SensorCalibration tmp = out;
    float fVal;
    if (_parseFloat(json, "ph_slope",   fVal) && fVal != 0.0f) tmp.ph_slope   = fVal;
    if (_parseFloat(json, "ph_offset",  fVal))                  tmp.ph_offset  = fVal;
    if (_parseFloat(json, "tds_factor", fVal) && fVal > 0.0f)  tmp.tds_factor = fVal;
    if (!tmp.isValid()) {
        LOG_ERROR("CFG", "parseCalibrationJson: invalid");
        return false;
    }
    out = tmp;
    return true;
}

// ----------------------------------------------------------------
// parsePhDoseConfigJson  (MỚI)
// JSON keys: measure_interval_s, session_duration_s, warmup_s,
//            base_pulse_ms, pulse_per_unit, max_pulse_ms
// ----------------------------------------------------------------
bool ConfigManager::parsePhDoseConfigJson(const char* json, PhDoseConfig& out) {
    if (!json) return false;
    PhDoseConfig tmp = out;
    int iVal;

    if (_parseInt(json, "base_pulse_ms",  iVal) && iVal >= 50 && iVal <= 3000)
        tmp.base_pulse_ms = (uint32_t)iVal;
    if (_parseInt(json, "pulse_per_unit", iVal) && iVal >= 100 && iVal <= 5000)
        tmp.pulse_per_unit = (uint32_t)iVal;
    if (_parseInt(json, "max_pulse_ms",   iVal) && iVal >= 100 && iVal <= 5000)
        tmp.max_pulse_ms = (uint32_t)iVal;

    // Seconds → ms
    if (_parseInt(json, "measure_interval_s", iVal) && iVal >= 60)
        tmp.measure_interval_ms = (uint32_t)iVal * 1000UL;
    if (_parseInt(json, "session_duration_s", iVal) && iVal >= 10)
        tmp.session_duration_ms = (uint32_t)iVal * 1000UL;
    if (_parseInt(json, "warmup_s", iVal) && iVal >= 0)
        tmp.warmup_ms = (uint32_t)iVal * 1000UL;

    // Noise / shock detection thresholds (float, đơn vị pH)
    float fVal;
    if (_parseFloat(json, "noise_threshold", fVal) && fVal >= 0.1f && fVal <= 2.0f)
        tmp.noise_threshold = fVal;
    if (_parseFloat(json, "shock_threshold", fVal) && fVal >= 0.1f && fVal <= 2.0f)
        tmp.shock_threshold = fVal;

    if (!tmp.isValid()) {
        LOG_ERROR("CFG", "parsePhDoseConfigJson: invalid values");
        return false;
    }
    out = tmp;
    return true;
}

// ================================================================
// APPLY FROM FIREBASE
// ================================================================
bool ConfigManager::applyControlConfig(const ControlConfig& cfg) {
    ConfigError err = ConfigValidator::validate(cfg);
    if (err != ConfigError::OK) {
        LOG_ERROR("CFG", "applyControlConfig rejected: %s", ConfigValidator::errorString(err));
        return false;
    }
    _ctrl = cfg;
    saveControlConfig(cfg);

    // Propagate — bỏ pidCtrl, chỉ còn hysteresis + phDoseCtrl
    hysteresisCtrl.setConfig(cfg);
    phDoseCtrl.setControlConfig(cfg);

    LOG_INFO("CFG", "ControlConfig applied: temp=[%.1f~%.1f] pH=[%.2f~%.2f] TDS=%.0f±%.0f",
             cfg.temp_min, cfg.temp_max,
             cfg.ph_min,   cfg.ph_max,
             cfg.tds_target, cfg.tds_tolerance);
    return true;
}

bool ConfigManager::applyPipelineConfig(const PipelineConfig& cfg) {
    if (cfg.temp_min >= cfg.temp_max || cfg.tds_min >= cfg.tds_max) {
        LOG_ERROR("CFG", "applyPipelineConfig: invalid range");
        return false;
    }
    _pipe = cfg;
    savePipelineConfig(cfg);
    dataPipeline.setConfig(cfg);
    LOG_INFO("CFG", "PipelineConfig applied: T=[%.1f~%.1f] TDS=[%.0f~%.0f]",
             cfg.temp_min, cfg.temp_max, cfg.tds_min, cfg.tds_max);
    return true;
}

bool ConfigManager::applyWaterSchedule(const WaterChangeSchedule& sched) {
    if (sched.pump_min_sec     < WATER_CHANGE_MIN_PUMP_SEC     ||
        sched.pump_out_max_sec > WATER_CHANGE_MAX_PUMP_OUT_SEC ||
        sched.pump_in_max_sec  > WATER_CHANGE_MAX_PUMP_IN_SEC  ||
        sched.pump_min_sec     >= sched.pump_out_max_sec        ||
        sched.pump_min_sec     >= sched.pump_in_max_sec) {
        LOG_ERROR("CFG", "applyWaterSchedule: invalid limits");
        return false;
    }
    if (sched.hour > 23 || sched.minute > 59 ||
        sched.pump_out_sec < sched.pump_min_sec ||
        sched.pump_in_sec  < sched.pump_min_sec) {
        LOG_ERROR("CFG", "applyWaterSchedule: invalid schedule values");
        return false;
    }
    _water = sched;
    saveWaterSchedule(sched);
    WaterChangeConfig wc = waterChangeManager.getConfig();
    wc.schedule_enabled = sched.enabled;
    wc.schedule_hour    = sched.hour;
    wc.schedule_minute  = sched.minute;
    wc.pump_out_sec     = sched.pump_out_sec;
    wc.pump_in_sec      = sched.pump_in_sec;
    waterChangeManager.setConfig(wc);
    LOG_INFO("CFG", "WaterSchedule applied: %s %02d:%02d out=%ds in=%ds",
             sched.enabled ? "ON" : "OFF", sched.hour, sched.minute,
             sched.pump_out_sec, sched.pump_in_sec);
    return true;
}

bool ConfigManager::applyCalibration(const SensorCalibration& calib) {
    if (!calib.isValid()) {
        LOG_ERROR("CFG", "applyCalibration rejected: invalid");
        return false;
    }
    _calib = calib;
    saveCalibration(calib);
    sensorManagerSetCalibration(calib.ph_slope, calib.ph_offset, calib.tds_factor);
    LOG_INFO("CFG", "Calibration applied: slope=%.4f offset=%.4f factor=%.4f",
             calib.ph_slope, calib.ph_offset, calib.tds_factor);
    return true;
}

// ----------------------------------------------------------------
// applyPhDoseConfig  (MỚI)
// ----------------------------------------------------------------
bool ConfigManager::applyPhDoseConfig(const PhDoseConfig& cfg) {
    if (!cfg.isValid()) {
        LOG_ERROR("CFG", "applyPhDoseConfig: invalid config");
        return false;
    }
    _dose = cfg;
    savePhDoseConfig(cfg);

    // Propagate ngay đến controller + session manager
    phDoseCtrl.setDoseConfig(cfg);
    phSessionMgr.setConfig(cfg);

    LOG_INFO("CFG", "PhDoseConfig applied: base=%lums per_unit=%lums max=%lums interval=%lus session=%lus warmup=%lus",
             (unsigned long)cfg.base_pulse_ms,
             (unsigned long)cfg.pulse_per_unit,
             (unsigned long)cfg.max_pulse_ms,
             (unsigned long)(cfg.measure_interval_ms / 1000),
             (unsigned long)(cfg.session_duration_ms / 1000),
             (unsigned long)(cfg.warmup_ms           / 1000));
    return true;
}

// ================================================================
// SERIAL HANDLER
// ================================================================
void ConfigManager::handleSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (_serialPos > 0) {
                _serialBuf[_serialPos] = '\0';
                _serialPos = 0;
                LOG_DEBUG("CFG", "Serial JSON: %s", _serialBuf);
                char typeBuf[16];
                if (!_findJsonValue(_serialBuf, "type", typeBuf, sizeof(typeBuf))) {
                    LOG_WARNING("CFG", "Serial JSON missing 'type'");
                    continue;
                }
                if (strcmp(typeBuf, "ctrl") == 0) {
                    ControlConfig tmp = _ctrl;
                    if (parseControlConfigJson(_serialBuf, tmp)) {
                        applyControlConfig(tmp);
                        Serial.println("{\"status\":\"ok\",\"type\":\"ctrl\"}");
                    } else {
                        Serial.println("{\"status\":\"error\",\"type\":\"ctrl\"}");
                    }
                } else if (strcmp(typeBuf, "pipeline") == 0) {
                    PipelineConfig tmp = _pipe;
                    if (parsePipelineConfigJson(_serialBuf, tmp)) {
                        applyPipelineConfig(tmp);
                        Serial.println("{\"status\":\"ok\",\"type\":\"pipeline\"}");
                    } else {
                        Serial.println("{\"status\":\"error\",\"type\":\"pipeline\"}");
                    }
                } else if (strcmp(typeBuf, "water") == 0) {
                    WaterChangeSchedule tmp = _water;
                    if (parseWaterScheduleJson(_serialBuf, tmp)) {
                        applyWaterSchedule(tmp);
                        Serial.println("{\"status\":\"ok\",\"type\":\"water\"}");
                    } else {
                        Serial.println("{\"status\":\"error\",\"type\":\"water\"}");
                    }
                } else if (strcmp(typeBuf, "dose") == 0) {
                    PhDoseConfig tmp = _dose;
                    if (parsePhDoseConfigJson(_serialBuf, tmp)) {
                        applyPhDoseConfig(tmp);
                        Serial.println("{\"status\":\"ok\",\"type\":\"dose\"}");
                    } else {
                        Serial.println("{\"status\":\"error\",\"type\":\"dose\"}");
                    }
                } else {
                    LOG_WARNING("CFG", "Serial JSON unknown type: %s", typeBuf);
                    Serial.println("{\"status\":\"error\",\"msg\":\"unknown type\"}");
                }
            }
        } else {
            if (_serialPos < SERIAL_BUF_SIZE - 1) _serialBuf[_serialPos++] = c;
            else { LOG_WARNING("CFG", "Serial buffer overflow"); _serialPos = 0; }
        }
    }
}