#pragma once
#include "type_definitions.h"
#include "circular_buffer.h"
#include <stddef.h>

// ================================================================
// data_pipeline.h
// Intelligent Aquarium v4.0
//
// Lọc dữ liệu raw → CleanReading qua 3 tầng:
//   Tầng 1: Range Gate   — loại NaN / out-of-range
//   Tầng 2: MAD Filter   — loại spike
//   Tầng 3: Shock Flag   — chỉ cảnh báo, không reject
//
// NGUYÊN TẮC QUAN TRỌNG:
//   - process() KHÔNG đọc cleanBuffer từ ngoài
//   - MAD filter chỉ đọc validRawBuffer bên trong (input gốc)
//   - Range fail → KHÔNG push validRawBuffer (giữ buffer sạch)
// ================================================================

// ----------------------------------------------------------------
// PIPELINE CONFIG — có thể thay đổi runtime qua setConfig()
// ----------------------------------------------------------------
struct PipelineConfig {
    // --- Range Gate (Tầng 1) ---
    float temp_min = 15.0f,  temp_max = 40.0f;
    float ph_min   =  4.0f,  ph_max   = 10.0f;
    float tds_min  =  1.0f,  tds_max  = 3000.0f;
    // ※ LOẠI BỎ: water_level_min, water_level_max

    // --- MAD Filter (Tầng 2) ---
    size_t mad_window_size  = 30;    // Kích thước validRawBuffer
    size_t mad_min_samples  = 10;    // Cần ít nhất n mẫu để tính MAD
    float  mad_threshold    = 3.5f;  // Ngưỡng z-score MAD
    float  mad_floor_temp   = 0.30f; // MAD tối thiểu cho nhiệt độ
    float  mad_floor_ph     = 0.08f; // MAD tối thiểu cho pH
    float  mad_floor_tds    = 3.0f;  // MAD tối thiểu cho TDS
    // ※ LOẠI BỎ: mad_floor_water

    // --- Shock Flag (Tầng 3) ---
    float shock_temp_delta = 3.0f;   // Thay đổi °C trong 1 chu kỳ → shock
    float shock_ph_delta   = 0.5f;   // Thay đổi pH trong 1 chu kỳ → shock
};

// ----------------------------------------------------------------
// DATA PIPELINE
// ----------------------------------------------------------------
class DataPipeline {
public:
    DataPipeline();

    // Xử lý 1 SensorReading raw → CleanReading
    // Gọi mỗi chu kỳ SENSOR_READ_INTERVAL_MS
    CleanReading process(const SensorReading& raw);

    // Reset toàn bộ state nội bộ (validRawBuffers, last_good, fallback)
    void reset();

    // Cập nhật config runtime (từ Firebase / Serial)
    void setConfig(const PipelineConfig& cfg);

    const PipelineConfig& getConfig() const { return _cfg; }

private:
    // ---- Config ----
    PipelineConfig _cfg;

    // ---- Valid Raw Buffers (MAD đọc từ đây, KHÔNG phải cleanBuffer) ----
    // Kích thước cố định = mad_window_size tối đa
    static constexpr size_t MAX_WINDOW = 30;
    CircularBuffer<float, MAX_WINDOW> _validTemp;
    CircularBuffer<float, MAX_WINDOW> _validPh;
    CircularBuffer<float, MAX_WINDOW> _validTds;

    // ---- Last Good Values (dùng khi fallback) ----
    float _lastGoodTemp;
    float _lastGoodPh;
    float _lastGoodTds;
    bool  _hasLastGoodTemp;
    bool  _hasLastGoodPh;
    bool  _hasLastGoodTds;

    // ---- Fallback Counters ----
    uint8_t _fallbackTemp;
    uint8_t _fallbackPh;
    uint8_t _fallbackTds;

    // ---- Previous Clean Values (dùng cho shock detection) ----
    float _prevCleanTemp;
    float _prevCleanPh;
    bool  _hasPrevCleanTemp;
    bool  _hasPrevCleanPh;

    // ---- Private helpers ----

    // Tính median của CircularBuffer (không sửa buffer gốc)
    float _median(const CircularBuffer<float, MAX_WINDOW>& buf) const;

    // Tầng 1+2+3 cho từng field
    struct FieldResult {
        float       value;
        DataSource  source;
        FieldStatus status;
        uint8_t     fallback_count;
    };

    FieldResult _processField(
        float raw,
        float rangeMin,
        float rangeMax,
        float madFloor,
        CircularBuffer<float, MAX_WINDOW>& validBuf,
        float& lastGood,
        bool&  hasLastGood,
        uint8_t& fallbackCount
    );
};
