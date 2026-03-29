#pragma once
#include "system_constants.h"
#include "control_config.h"
#include "data_pipeline.h"
#include "type_definitions.h"
#include <Arduino.h>

// ================================================================
// config_manager.h
// Intelligent Aquarium v4.0
//
// Quản lý 3 loại config:
//   1. ControlConfig    — thông số người dùng (temp, pH, TDS, PID)
//   2. PipelineConfig   — thông số pipeline (range, MAD, shock)
//   3. WaterChangeSchedule — lịch thay nước
//   4. SensorCalibration — hệ số hiệu chuẩn pH & TDS
//
// Lưu trữ: ESP32 NVS (Preferences)
// Nhận từ:  Serial (JSON) hoặc Firebase (callback)
// ================================================================

// ----------------------------------------------------------------
// NVS NAMESPACE KEYS
// ----------------------------------------------------------------
#define NVS_NAMESPACE_CTRL      "aq_ctrl"
#define NVS_NAMESPACE_PIPELINE  "aq_pipe"
#define NVS_NAMESPACE_WATER     "aq_water"
#define NVS_NAMESPACE_CALIB     "aq_calib"

// ----------------------------------------------------------------
// SENSOR CALIBRATION STRUCT
// ----------------------------------------------------------------
struct SensorCalibration {
    float ph_slope  = PH_CALIB_SLOPE_DEFAULT;   // pH = slope × V + offset
    float ph_offset = PH_CALIB_OFFSET_DEFAULT;
    float tds_factor = TDS_CALIB_FACTOR_DEFAULT; // nhân sau công thức polynomial

    // Validate: slope != 0, factor > 0
    bool isValid() const {
        return (ph_slope != 0.0f) && (tds_factor > 0.0f);
    }
};

// ----------------------------------------------------------------
// CONFIG MANAGER
// ----------------------------------------------------------------
class ConfigManager {
public:
    ConfigManager();

    // ---- Init ----
    // Gọi trong setup(). Load từ NVS, nếu không có → dùng default.
    void begin();

    // ---- NVS persistence ----
    bool loadControlConfig(ControlConfig& out);
    bool saveControlConfig(const ControlConfig& cfg);

    bool loadPipelineConfig(PipelineConfig& out);
    bool savePipelineConfig(const PipelineConfig& cfg);

    bool loadWaterSchedule(WaterChangeSchedule& out);
    bool saveWaterSchedule(const WaterChangeSchedule& sched);

    bool loadCalibration(SensorCalibration& out);
    bool saveCalibration(const SensorCalibration& calib);

    // ---- JSON parsers ----
    // Nhận chuỗi JSON (từ Serial hoặc Firebase stream value),
    // parse vào struct, validate rồi mới áp dụng.
    // Trả về true nếu thành công.
    bool parseControlConfigJson(const char* json, ControlConfig& out);
    bool parsePipelineConfigJson(const char* json, PipelineConfig& out);
    bool parseWaterScheduleJson(const char* json, WaterChangeSchedule& out);
    bool parseCalibrationJson(const char* json, SensorCalibration& out);

    // ---- Serial handler ----
    // Gọi trong loop(). Đọc JSON từ Serial, detect loại config,
    // parse và lưu vào NVS. Format:
    //   {"type":"ctrl","temp_min":25,"temp_max":28,...}
    //   {"type":"pipeline","temp_min":15,"temp_max":40,...}
    //   {"type":"water","enabled":true,"hour":6,"minute":0,...}
    void handleSerial();

    // ---- Getters (bản đang dùng trong RAM) ----
    const ControlConfig&        getControlConfig()   const { return _ctrl; }
    const PipelineConfig&       getPipelineConfig()  const { return _pipe; }
    const WaterChangeSchedule&  getWaterSchedule()   const { return _water; }
    const SensorCalibration&    getCalibration()     const { return _calib; }

    // ---- Apply từ Firebase / Serial ----
    // Validate → cập nhật RAM → lưu NVS → propagate ngay đến controller/pipeline
    // Caller KHÔNG cần gọi thêm setConfig() trên controller sau khi apply thành công.
    bool applyControlConfig(const ControlConfig& cfg);
    bool applyPipelineConfig(const PipelineConfig& cfg);
    bool applyWaterSchedule(const WaterChangeSchedule& sched);
    bool applyCalibration(const SensorCalibration& calib);

private:
    ControlConfig       _ctrl;
    PipelineConfig      _pipe;
    WaterChangeSchedule _water;
    SensorCalibration   _calib;

    // Serial input buffer
    static constexpr size_t SERIAL_BUF_SIZE = 512;
    char   _serialBuf[SERIAL_BUF_SIZE];
    size_t _serialPos;

    // Internal helpers
    bool _parseFloat(const char* json, const char* key, float& out);
    bool _parseInt(const char* json, const char* key, int& out);
    bool _parseBool(const char* json, const char* key, bool& out);

    // Tìm value string của key trong JSON phẳng (không lồng nhau)
    // Viết tay để tránh phụ thuộc ArduinoJson library
    bool _findJsonValue(const char* json, const char* key,
                        char* valueBuf, size_t bufSize);
};

// Global singleton
extern ConfigManager configManager;