#include "config_manager.h"
#include "logger.h"
#include "hysteresis_controller.h"
#include "pid_controller.h"
#include "data_pipeline.h"
#include "water_change_manager.h"
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Forward-declared globals (defined in their respective .cpp files)
extern HysteresisController hysteresisCtrl;
extern PidController        pidCtrl;
extern DataPipeline         dataPipeline;
extern WaterChangeManager   waterChangeManager;

// ================================================================
// config_manager.cpp
// Intelligent Aquarium v4.0
// ================================================================

// Global singleton
ConfigManager configManager;

// ================================================================
// Minimal JSON parser — không dùng ArduinoJson để tiết kiệm RAM.
// Chỉ hỗ trợ JSON phẳng 1 cấp: {"key":value, "key2":"str", ...}
// ================================================================

// Tìm giá trị của key trong JSON phẳng, trả về dạng chuỗi thô.
bool ConfigManager::_findJsonValue(const char* json, const char* key,
                                    char* valueBuf, size_t bufSize) {
    if (!json || !key || !valueBuf) return false;

    // Tìm `"key"` trong json
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);

    const char* pos = strstr(json, searchKey);
    if (!pos) return false;

    // Bỏ qua key và dấu ":"
    pos += strlen(searchKey);
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;

    // Đọc value (có thể có nháy đôi hoặc không)
    size_t i = 0;
    if (*pos == '"') {
        // String value
        pos++;
        while (*pos && *pos != '"' && i < bufSize - 1) {
            valueBuf[i++] = *pos++;
        }
    } else {
        // Numeric / bool value
        while (*pos && *pos != ',' && *pos != '}' && *pos != ' ' && i < bufSize - 1) {
            valueBuf[i++] = *pos++;
        }
    }
    valueBuf[i] = '\0';
    return (i > 0);
}

bool ConfigManager::_parseFloat(const char* json, const char* key, float& out) {
    char buf[32];
    if (!_findJsonValue(json, key, buf, sizeof(buf))) return false;
    out = atof(buf);
    return true;
}

bool ConfigManager::_parseInt(const char* json, const char* key, int& out) {
    char buf[32];
    if (!_findJsonValue(json, key, buf, sizeof(buf))) return false;
    out = atoi(buf);
    return true;
}

bool ConfigManager::_parseBool(const char* json, const char* key, bool& out) {
    char buf[8];
    if (!_findJsonValue(json, key, buf, sizeof(buf))) return false;
    out = (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0);
    return true;
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

    LOG_INFO("CFG", "begin: ctrl=%s pipe=%s water=%s",
             ctrlOk ? "NVS" : "default",
             pipeOk ? "NVS" : "default",
             waterOk ? "NVS" : "default");
}

// ================================================================
// NVS — CONTROL CONFIG
// ================================================================
bool ConfigManager::loadControlConfig(ControlConfig& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_CTRL, true)) return false;

    // Kiểm tra marker để biết có data hay không
    if (!prefs.isKey("saved")) { prefs.end(); return false; }

    out.temp_min      = prefs.getFloat("temp_min",      out.temp_min);
    out.temp_max      = prefs.getFloat("temp_max",      out.temp_max);
    out.ph_min        = prefs.getFloat("ph_min",        out.ph_min);
    out.ph_max        = prefs.getFloat("ph_max",        out.ph_max);
    out.tds_target    = prefs.getFloat("tds_target",    out.tds_target);
    out.tds_tolerance = prefs.getFloat("tds_tol",       out.tds_tolerance);
    out.pid_kp        = prefs.getFloat("pid_kp",        out.pid_kp);
    out.pid_ki        = prefs.getFloat("pid_ki",        out.pid_ki);
    out.pid_kd        = prefs.getFloat("pid_kd",        out.pid_kd);
    prefs.end();

    ConfigError err = ConfigValidator::validate(out);
    if (err != ConfigError::OK) {
        LOG_WARNING("CFG", "NVS ctrl invalid (%s) → default", ConfigValidator::errorString(err));
        out = ControlConfig{};
        return false;
    }
    return true;
}

bool ConfigManager::saveControlConfig(const ControlConfig& cfg) {
    ConfigError err = ConfigValidator::validate(cfg);
    if (err != ConfigError::OK) {
        LOG_ERROR("CFG", "saveControlConfig rejected: %s", ConfigValidator::errorString(err));
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_CTRL, false)) return false;

    prefs.putFloat("temp_min",   cfg.temp_min);
    prefs.putFloat("temp_max",   cfg.temp_max);
    prefs.putFloat("ph_min",     cfg.ph_min);
    prefs.putFloat("ph_max",     cfg.ph_max);
    prefs.putFloat("tds_target", cfg.tds_target);
    prefs.putFloat("tds_tol",    cfg.tds_tolerance);
    prefs.putFloat("pid_kp",     cfg.pid_kp);
    prefs.putFloat("pid_ki",     cfg.pid_ki);
    prefs.putFloat("pid_kd",     cfg.pid_kd);
    prefs.putBool("saved",       true);
    prefs.end();

    LOG_INFO("CFG", "ControlConfig saved to NVS");
    return true;
}

// ================================================================
// NVS — PIPELINE CONFIG
// ================================================================
bool ConfigManager::loadPipelineConfig(PipelineConfig& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_PIPELINE, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }

    out.temp_min         = prefs.getFloat("t_min",       out.temp_min);
    out.temp_max         = prefs.getFloat("t_max",       out.temp_max);
    out.ph_min           = prefs.getFloat("ph_min",      out.ph_min);
    out.ph_max           = prefs.getFloat("ph_max",      out.ph_max);
    out.tds_min          = prefs.getFloat("tds_min",     out.tds_min);
    out.tds_max          = prefs.getFloat("tds_max",     out.tds_max);
    out.mad_window_size  = prefs.getUInt("mad_win",      out.mad_window_size);
    out.mad_min_samples  = prefs.getUInt("mad_minsamp",  out.mad_min_samples);
    out.mad_threshold    = prefs.getFloat("mad_thr",     out.mad_threshold);
    out.mad_floor_temp   = prefs.getFloat("mad_ft",      out.mad_floor_temp);
    out.mad_floor_ph     = prefs.getFloat("mad_fph",     out.mad_floor_ph);
    out.mad_floor_tds    = prefs.getFloat("mad_ftds",    out.mad_floor_tds);
    out.shock_temp_delta = prefs.getFloat("shk_t",       out.shock_temp_delta);
    out.shock_ph_delta   = prefs.getFloat("shk_ph",      out.shock_ph_delta);
    prefs.end();
    return true;
}

bool ConfigManager::savePipelineConfig(const PipelineConfig& cfg) {
    // Basic sanity checks
    if (cfg.temp_min >= cfg.temp_max || cfg.ph_min >= cfg.ph_max ||
        cfg.tds_min  >= cfg.tds_max  || cfg.mad_window_size < 5 ||
        cfg.mad_threshold <= 0.0f) {
        LOG_ERROR("CFG", "savePipelineConfig: invalid values");
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_PIPELINE, false)) return false;

    prefs.putFloat("t_min",      cfg.temp_min);
    prefs.putFloat("t_max",      cfg.temp_max);
    prefs.putFloat("ph_min",     cfg.ph_min);
    prefs.putFloat("ph_max",     cfg.ph_max);
    prefs.putFloat("tds_min",    cfg.tds_min);
    prefs.putFloat("tds_max",    cfg.tds_max);
    prefs.putUInt("mad_win",     cfg.mad_window_size);
    prefs.putUInt("mad_minsamp", cfg.mad_min_samples);
    prefs.putFloat("mad_thr",    cfg.mad_threshold);
    prefs.putFloat("mad_ft",     cfg.mad_floor_temp);
    prefs.putFloat("mad_fph",    cfg.mad_floor_ph);
    prefs.putFloat("mad_ftds",   cfg.mad_floor_tds);
    prefs.putFloat("shk_t",      cfg.shock_temp_delta);
    prefs.putFloat("shk_ph",     cfg.shock_ph_delta);
    prefs.putBool("saved",       true);
    prefs.end();

    LOG_INFO("CFG", "PipelineConfig saved to NVS");
    return true;
}

// ================================================================
// NVS — WATER CHANGE SCHEDULE
// ================================================================
bool ConfigManager::loadWaterSchedule(WaterChangeSchedule& out) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_WATER, true)) return false;
    if (!prefs.isKey("saved")) { prefs.end(); return false; }

    out.enabled      = prefs.getBool("enabled",   out.enabled);
    out.hour         = prefs.getUChar("hour",      out.hour);
    out.minute       = prefs.getUChar("minute",    out.minute);
    out.pump_out_sec = prefs.getUShort("pout_sec", out.pump_out_sec);
    out.pump_in_sec  = prefs.getUShort("pin_sec",  out.pump_in_sec);
    out.last_run_day = prefs.getULong("last_day", out.last_run_day);
    out.last_run_ts  = prefs.getULong("last_ts",  out.last_run_ts);
    prefs.end();
    LOG_INFO("CFG", "WaterSchedule loaded: enabled=%d %02d:%02d out=%ds in=%ds last_day=%lu last_ts=%lu",
             out.enabled, out.hour, out.minute,
             out.pump_out_sec, out.pump_in_sec,
             (unsigned long)out.last_run_day,
             (unsigned long)out.last_run_ts);
    return true;
}

bool ConfigManager::saveWaterSchedule(const WaterChangeSchedule& sched) {
    if (sched.hour > 23 || sched.minute > 59 ||
        sched.pump_out_sec < 10 || sched.pump_in_sec < 10) {
        LOG_ERROR("CFG", "saveWaterSchedule: invalid values h=%d m=%d out=%d in=%d",
                  sched.hour, sched.minute, sched.pump_out_sec, sched.pump_in_sec);
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE_WATER, false)) return false;

    prefs.putBool("enabled",    sched.enabled);
    prefs.putUChar("hour",      sched.hour);
    prefs.putUChar("minute",    sched.minute);
    prefs.putUShort("pout_sec", sched.pump_out_sec);
    prefs.putUShort("pin_sec",  sched.pump_in_sec);
    prefs.putULong("last_day", sched.last_run_day);
    prefs.putULong("last_ts",  sched.last_run_ts);
    prefs.putBool("saved",      true);
    prefs.end();

    LOG_INFO("CFG", "WaterSchedule saved: enabled=%d %02d:%02d out=%ds in=%ds last_day=%lu last_ts=%lu",
             sched.enabled, sched.hour, sched.minute,
             sched.pump_out_sec, sched.pump_in_sec,
             (unsigned long)sched.last_run_day,
             (unsigned long)sched.last_run_ts);
    return true;
}

// ================================================================
// JSON PARSERS
// ================================================================
bool ConfigManager::parseControlConfigJson(const char* json, ControlConfig& out) {
    ControlConfig tmp = out;  // Bắt đầu từ giá trị hiện tại

    _parseFloat(json, "temp_min",      tmp.temp_min);
    _parseFloat(json, "temp_max",      tmp.temp_max);
    _parseFloat(json, "ph_min",        tmp.ph_min);
    _parseFloat(json, "ph_max",        tmp.ph_max);
    _parseFloat(json, "tds_target",    tmp.tds_target);
    _parseFloat(json, "tds_tolerance", tmp.tds_tolerance);
    _parseFloat(json, "pid_kp",        tmp.pid_kp);
    _parseFloat(json, "pid_ki",        tmp.pid_ki);
    _parseFloat(json, "pid_kd",        tmp.pid_kd);

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

    // Keys dùng prefix "range" để tránh collision với ControlConfig (temp_min/max)
    _parseFloat(json, "temp_range_min",   tmp.temp_min);
    _parseFloat(json, "temp_range_max",   tmp.temp_max);
    _parseFloat(json, "ph_range_min",     tmp.ph_min);
    _parseFloat(json, "ph_range_max",     tmp.ph_max);
    _parseFloat(json, "tds_range_min",    tmp.tds_min);
    _parseFloat(json, "tds_range_max",    tmp.tds_max);
    _parseFloat(json, "mad_threshold",    tmp.mad_threshold);
    _parseFloat(json, "mad_floor_temp",   tmp.mad_floor_temp);
    _parseFloat(json, "mad_floor_ph",     tmp.mad_floor_ph);
    _parseFloat(json, "mad_floor_tds",    tmp.mad_floor_tds);
    _parseFloat(json, "shock_temp_delta", tmp.shock_temp_delta);
    _parseFloat(json, "shock_ph_delta",   tmp.shock_ph_delta);

    int iVal;
    if (_parseInt(json, "mad_window_size", iVal) && iVal >= 5 && iVal <= 30)
        tmp.mad_window_size = (size_t)iVal;
    if (_parseInt(json, "mad_min_samples", iVal) && iVal >= 1 && iVal <= (int)tmp.mad_window_size)
        tmp.mad_min_samples = (size_t)iVal;

    // Sanity check
    if (tmp.temp_min >= tmp.temp_max || tmp.ph_min >= tmp.ph_max ||
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
    if (_parseBool(json, "enabled", bVal))   tmp.enabled = bVal;
    if (_parseInt(json, "hour",     iVal) && iVal >= 0 && iVal <= 23)  tmp.hour   = (uint8_t)iVal;
    if (_parseInt(json, "minute",   iVal) && iVal >= 0 && iVal <= 59)  tmp.minute = (uint8_t)iVal;
    if (_parseInt(json, "pump_out_sec", iVal) && iVal >= 10) tmp.pump_out_sec = (uint16_t)iVal;
    if (_parseInt(json, "pump_in_sec",  iVal) && iVal >= 10) tmp.pump_in_sec  = (uint16_t)iVal;

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

    // Propagate ngay đến các controller — tránh stale config sau khi Firebase update
    hysteresisCtrl.setConfig(cfg);
    pidCtrl.setConfig(cfg);

    LOG_INFO("CFG", "ControlConfig applied → hysteresis+PID updated"
             " (temp=[%.1f~%.1f] pH=[%.2f~%.2f] Kp=%.2f Ki=%.3f Kd=%.3f)",
             cfg.temp_min, cfg.temp_max,
             cfg.ph_min,   cfg.ph_max,
             cfg.pid_kp,   cfg.pid_ki, cfg.pid_kd);
    return true;
}

bool ConfigManager::applyPipelineConfig(const PipelineConfig& cfg) {
    if (cfg.temp_min >= cfg.temp_max || cfg.ph_min >= cfg.ph_max) {
        LOG_ERROR("CFG", "applyPipelineConfig: invalid range");
        return false;
    }
    _pipe = cfg;
    savePipelineConfig(cfg);

    // Propagate ngay đến pipeline — tránh stale filter config
    dataPipeline.setConfig(cfg);

    LOG_INFO("CFG", "PipelineConfig applied → dataPipeline updated"
             " (T=[%.1f~%.1f] pH=[%.1f~%.1f] TDS=[%.0f~%.0f] MAD thr=%.2f)",
             cfg.temp_min, cfg.temp_max,
             cfg.ph_min,   cfg.ph_max,
             cfg.tds_min,  cfg.tds_max,
             cfg.mad_threshold);
    return true;
}

bool ConfigManager::applyWaterSchedule(const WaterChangeSchedule& sched) {
    if (sched.hour > 23 || sched.minute > 59 ||
        sched.pump_out_sec < 10 || sched.pump_in_sec < 10) {
        LOG_ERROR("CFG", "applyWaterSchedule: invalid h=%d m=%d out=%d in=%d",
                  sched.hour, sched.minute, sched.pump_out_sec, sched.pump_in_sec);
        return false;
    }
    _water = sched;
    saveWaterSchedule(sched);

    // Propagate ngay đến WaterChangeManager
    // Giữ lại last_run_day/ts hiện tại — không bị overwrite khi update schedule
    WaterChangeConfig wc = waterChangeManager.getConfig();
    wc.schedule_enabled = sched.enabled;
    wc.schedule_hour    = sched.hour;
    wc.schedule_minute  = sched.minute;
    wc.pump_out_sec     = sched.pump_out_sec;
    wc.pump_in_sec      = sched.pump_in_sec;
    waterChangeManager.setConfig(wc);

    LOG_INFO("CFG", "WaterSchedule applied → waterChangeManager updated"
             " (schedule=%s %02d:%02d out=%ds in=%ds)",
             sched.enabled ? "ON" : "OFF",
             sched.hour, sched.minute,
             sched.pump_out_sec, sched.pump_in_sec);
    return true;
}

// ================================================================
// SERIAL HANDLER
// Đọc từng byte từ Serial, ghép thành JSON hoàn chỉnh (đóng '}'),
// detect "type" field rồi dispatch đến parser phù hợp.
// ================================================================
void ConfigManager::handleSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            // Kết thúc dòng — xử lý buffer nếu có nội dung
            if (_serialPos > 0) {
                _serialBuf[_serialPos] = '\0';
                _serialPos = 0;

                LOG_DEBUG("CFG", "Serial JSON: %s", _serialBuf);

                // Detect type
                char typeBuf[16];
                if (!_findJsonValue(_serialBuf, "type", typeBuf, sizeof(typeBuf))) {
                    LOG_WARNING("CFG", "Serial JSON missing 'type' field");
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
                } else {
                    LOG_WARNING("CFG", "Serial JSON unknown type: %s", typeBuf);
                    Serial.println("{\"status\":\"error\",\"msg\":\"unknown type\"}");
                }
            }
        } else {
            // Tích lũy ký tự vào buffer
            if (_serialPos < SERIAL_BUF_SIZE - 1) {
                _serialBuf[_serialPos++] = c;
            } else {
                // Buffer đầy → reset
                LOG_WARNING("CFG", "Serial buffer overflow, resetting");
                _serialPos = 0;
            }
        }
    }
}