#pragma once
#include <stdint.h>
#include <stdbool.h>

// ================================================================
// type_definitions.h
// Intelligent Aquarium v4.0
// Tất cả kiểu dữ liệu dùng chung giữa các module
// ================================================================

// ----------------------------------------------------------------
// DATA SOURCE — nguồn gốc giá trị sau pipeline
// ----------------------------------------------------------------
enum class DataSource : uint8_t {
    MEASURED,          // Đo trực tiếp, qua đủ filter
    FALLBACK_LAST,     // Dùng giá trị hợp lệ cuối
    FALLBACK_MEDIAN,   // Dùng median của validRawBuffer (MAD outlier)
    FALLBACK_DEFAULT   // Không có lịch sử → dùng giá trị mặc định
};

// ----------------------------------------------------------------
// FIELD STATUS — trạng thái từng field sau pipeline
// ----------------------------------------------------------------
enum class FieldStatus : uint8_t {
    OK,             // Dữ liệu sạch, đo bình thường
    OUT_OF_RANGE,   // Vượt range gate, loại bỏ
    MAD_OUTLIER,    // Bị MAD filter phát hiện là spike
    SENSOR_ERROR    // Sensor không phản hồi / NaN / lỗi phần cứng
};

// ----------------------------------------------------------------
// SENSOR READING — raw từ phần cứng (trước pipeline)
// ※ Không còn water_level_cm (bỏ HC-SR04)
// ----------------------------------------------------------------
struct SensorReading {
    uint32_t timestamp;   // millis() lúc đọc
    float temperature;    // °C
    float ph;             // đơn vị pH
    float tds;            // ppm
};

// ----------------------------------------------------------------
// CLEAN READING — output sau data pipeline
// ----------------------------------------------------------------
struct CleanReading {
    uint32_t timestamp;

    // Giá trị đã lọc (hoặc fallback)
    float temperature;
    float ph;
    float tds;

    // Nguồn gốc từng field
    DataSource source_temperature;
    DataSource source_ph;
    DataSource source_tds;

    // Trạng thái từng field
    FieldStatus status_temperature;
    FieldStatus status_ph;
    FieldStatus status_tds;

    // Shock flags (chỉ cảnh báo, không reject)
    bool shock_temperature;
    bool shock_ph;

    // Số chu kỳ liên tiếp dùng fallback
    uint8_t fallback_count_temp;
    uint8_t fallback_count_ph;
    uint8_t fallback_count_tds;

    // ---- Helpers ----

    // Tất cả field đều MEASURED (không có fallback nào)
    bool is_fully_clean() const {
        return (source_temperature == DataSource::MEASURED)
            && (source_ph         == DataSource::MEASURED)
            && (source_tds        == DataSource::MEASURED);
    }

    // Có bất kỳ shock flag nào
    bool has_shock() const {
        return shock_temperature || shock_ph;
    }

    // Temperature hợp lệ (không phải fallback default)
    bool temp_valid() const {
        return status_temperature != FieldStatus::SENSOR_ERROR
            && source_temperature != DataSource::FALLBACK_DEFAULT;
    }

    // pH hợp lệ
    bool ph_valid() const {
        return status_ph != FieldStatus::SENSOR_ERROR
            && source_ph != DataSource::FALLBACK_DEFAULT;
    }

    // TDS hợp lệ
    bool tds_valid() const {
        return status_tds != FieldStatus::SENSOR_ERROR
            && source_tds != DataSource::FALLBACK_DEFAULT;
    }
};

// ----------------------------------------------------------------
// WATER CHANGE STATE — trạng thái quy trình thay nước
// ----------------------------------------------------------------
enum class WaterChangeState : uint8_t {
    IDLE,         // Không hoạt động
    PUMPING_OUT,  // Đang bơm nước ra (relay pump_out ON)
    PUMPING_IN,   // Đang bơm nước vào (relay pump_in ON)
    DONE          // Vừa hoàn thành, chờ reset về IDLE
};

// ----------------------------------------------------------------
// WATER CHANGE SCHEDULE — lịch thay nước tự động
// ----------------------------------------------------------------
struct WaterChangeSchedule {
    bool     enabled       = false;  // Bật/tắt lịch tự động
    uint8_t  hour          = 6;      // Giờ chạy (0-23)
    uint8_t  minute        = 0;      // Phút chạy (0-59)
    uint16_t pump_out_sec  = 30;     // Thời gian bơm ra (giây)
    uint16_t pump_in_sec   = 60;     // Thời gian bơm vào (giây)
    uint32_t last_run_day  = 0;      // epoch/86400 — tránh chạy 2 lần/ngày
    uint32_t last_run_ts   = 0;      // Unix timestamp (UTC) lúc hoàn thành — web dùng để hiển thị ngày giờ phút

    // Giới hạn runtime — Admin config qua Firebase (thay vì hard-code macro)
    // Default lấy từ WATER_CHANGE_MIN/MAX constants khi load lần đầu
    uint16_t pump_min_sec     = 10;   // Tối thiểu bơm (giây) — áp dụng cho cả out và in
    uint16_t pump_out_max_sec = 300;  // Tối đa bơm ra (giây)
    uint16_t pump_in_max_sec  = 600;  // Tối đa bơm vào (giây)
};