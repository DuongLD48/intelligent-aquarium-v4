#pragma once
#include "system_constants.h"
#include "control_config.h"
#include "data_pipeline.h"
#include "ph_dose_controller.h"
#include "type_definitions.h"
#include <Arduino.h>

// ================================================================
// config_manager.h
// Intelligent Aquarium v4.1
//
// Quản lý config:
//   1. ControlConfig       — temp, pH range, TDS (KHÔNG còn PID)
//   2. PipelineConfig      — range gate, MAD, shock
//   3. WaterChangeSchedule — lịch thay nước
//   4. SensorCalibration   — hệ số hiệu chuẩn pH & TDS
//   5. PhDoseConfig        — linear dose controller (MỚI)
// ================================================================

// ----------------------------------------------------------------
// NVS NAMESPACE KEYS
// ----------------------------------------------------------------
#define NVS_NAMESPACE_CTRL      "aq_ctrl"
#define NVS_NAMESPACE_PIPELINE  "aq_pipe"
#define NVS_NAMESPACE_WATER     "aq_water"
#define NVS_NAMESPACE_CALIB     "aq_calib"
#define NVS_NAMESPACE_DOSE      "aq_phdose"   // MỚI

// ----------------------------------------------------------------
// SENSOR CALIBRATION
// ----------------------------------------------------------------
struct SensorCalibration {
    float ph_slope   = PH_CALIB_SLOPE_DEFAULT;
    float ph_offset  = PH_CALIB_OFFSET_DEFAULT;
    float tds_factor = TDS_CALIB_FACTOR_DEFAULT;

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

    bool loadPhDoseConfig(PhDoseConfig& out);      // MỚI
    bool savePhDoseConfig(const PhDoseConfig& cfg); // MỚI

    // ---- JSON parsers ----
    bool parseControlConfigJson(const char* json, ControlConfig& out);
    bool parsePipelineConfigJson(const char* json, PipelineConfig& out);
    bool parseWaterScheduleJson(const char* json, WaterChangeSchedule& out);
    bool parseCalibrationJson(const char* json, SensorCalibration& out);
    bool parsePhDoseConfigJson(const char* json, PhDoseConfig& out); // MỚI

    // ---- Serial handler ----
    void handleSerial();

    // ---- Getters ----
    const ControlConfig&        getControlConfig()  const { return _ctrl;  }
    const PipelineConfig&       getPipelineConfig() const { return _pipe;  }
    const WaterChangeSchedule&  getWaterSchedule()  const { return _water; }
    const SensorCalibration&    getCalibration()    const { return _calib; }
    const PhDoseConfig&         getPhDoseConfig()   const { return _dose;  } // MỚI

    // ---- Apply từ Firebase / Serial ----
    bool applyControlConfig(const ControlConfig& cfg);
    bool applyPipelineConfig(const PipelineConfig& cfg);
    bool applyWaterSchedule(const WaterChangeSchedule& sched);
    bool applyCalibration(const SensorCalibration& calib);
    bool applyPhDoseConfig(const PhDoseConfig& cfg); // MỚI

private:
    ControlConfig       _ctrl;
    PipelineConfig      _pipe;
    WaterChangeSchedule _water;
    SensorCalibration   _calib;
    PhDoseConfig        _dose;  // MỚI

    static constexpr size_t SERIAL_BUF_SIZE = 512;
    char   _serialBuf[SERIAL_BUF_SIZE];
    size_t _serialPos;

    bool _parseFloat(const char* json, const char* key, float& out);
    bool _parseInt(const char* json, const char* key, int& out);
    bool _parseBool(const char* json, const char* key, bool& out);
    bool _findJsonValue(const char* json, const char* key,
                        char* valueBuf, size_t bufSize);
};

extern ConfigManager configManager;
