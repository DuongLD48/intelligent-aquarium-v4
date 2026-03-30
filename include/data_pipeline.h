#pragma once
#include "type_definitions.h"
#include "circular_buffer.h"
#include <stddef.h>
#include <math.h>

// ================================================================
// data_pipeline.h
// Intelligent Aquarium v4.2
//
// 3 tầng filter cho nhiệt độ và TDS:
//   Tầng 1: Range Gate   — loại NaN / out-of-range
//   Tầng 2: MAD Filter   — loại spike
//   Tầng 3: Shock Flag   — chỉ cảnh báo, không reject (temp only)
//
// v4.2: Bỏ pH khỏi pipeline — pH đo qua PhSessionManager
// ================================================================

// ================================================================
// data_pipeline.h
// Intelligent Aquarium v4.2
//
// 3 tầng filter cho nhiệt độ và TDS:
//   Tầng 1: Range Gate   — loại NaN / out-of-range
//   Tầng 2: MAD Filter   — loại spike
//   Tầng 3: Shock Flag   — chỉ cảnh báo, không reject (temp only)
//
// v4.2: Bỏ pH khỏi pipeline — pH đo qua PhSessionManager
// ================================================================

struct PipelineConfig {
    float temp_min = 15.0f,  temp_max = 40.0f;
    float tds_min  =  1.0f,  tds_max  = 3000.0f;

    size_t mad_window_size  = 30;
    size_t mad_min_samples  = 10;
    float  mad_threshold    = 3.5f;
    float  mad_floor_temp   = 0.30f;
    float  mad_floor_tds    = 3.0f;

    float shock_temp_delta = 3.0f;
};

class DataPipeline {
public:
    DataPipeline();

    CleanReading process(const SensorReading& raw);
    void reset();
    void setConfig(const PipelineConfig& cfg);
    const PipelineConfig& getConfig() const { return _cfg; }

private:
    PipelineConfig _cfg;

    static constexpr size_t MAX_WINDOW = 30;

    CircularBuffer<float, MAX_WINDOW> _validTemp;
    CircularBuffer<float, MAX_WINDOW> _validTds;

    float  _lastGoodTemp, _lastGoodTds;
    bool   _hasLastGoodTemp, _hasLastGoodTds;
    uint8_t _fallbackTemp, _fallbackTds;

    float  _prevCleanTemp;
    bool   _hasPrevCleanTemp;

    struct FieldResult {
        float       value;
        DataSource  source;
        FieldStatus status;
        uint8_t     fallback_count;
    };

    FieldResult _processField(
        float raw,
        float rangeMin, float rangeMax,
        float madFloor,
        CircularBuffer<float, MAX_WINDOW>& validBuf,
        float& lastGood, bool& hasLastGood,
        uint8_t& fallbackCount
    );

    float _median(const CircularBuffer<float, MAX_WINDOW>& buf) const;
};

extern DataPipeline dataPipeline;