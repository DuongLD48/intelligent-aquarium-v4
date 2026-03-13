#pragma once
#include <stdint.h>

// ================================================================
// control_config.h
// Intelligent Aquarium v4.0
//
// ControlConfig — thông số vận hành do User cài đặt:
//   nhiệt độ mục tiêu, pH setpoint, TDS target, PID gains
//
// ConfigValidator — kiểm tra tính hợp lệ trước khi áp dụng
// ================================================================

// ----------------------------------------------------------------
// CONFIG ERROR ENUM
// ----------------------------------------------------------------
enum class ConfigError : uint8_t {
    OK = 0,

    // Temperature
    TEMP_MIN_TOO_LOW,        // temp_min < 10
    TEMP_MAX_TOO_HIGH,       // temp_max > 40
    TEMP_RANGE_TOO_NARROW,   // temp_max - temp_min < 2
    TEMP_MIN_GE_MAX,         // temp_min >= temp_max

    // pH
    PH_MIN_TOO_LOW,          // ph_min < 4.0
    PH_MAX_TOO_HIGH,         // ph_max > 10.0
    PH_RANGE_TOO_NARROW,     // ph_max - ph_min < 0.3
    PH_MIN_GE_MAX,           // ph_min >= ph_max

    // TDS
    TDS_TARGET_TOO_LOW,      // tds_target < 10
    TDS_TARGET_TOO_HIGH,     // tds_target > 2000
    TDS_TOLERANCE_INVALID,   // tds_tolerance <= 0

    // PID
    PID_KP_NEGATIVE,
    PID_KI_NEGATIVE,
    PID_KD_NEGATIVE,
};

// ----------------------------------------------------------------
// CONTROL CONFIG — thông số người dùng cài đặt
// ※ Đã bỏ water_min/max_target (không còn cảm biến mực nước)
// ----------------------------------------------------------------
struct ControlConfig {
    // --- Nhiệt độ ---
    float temp_min = 25.0f;   // °C — bật heater khi dưới
    float temp_max = 28.0f;   // °C — bật cooler khi trên

    // --- pH ---
    float ph_min = 6.5f;      // pH — bật pH_UP khi dưới
    float ph_max = 7.5f;      // pH — bật pH_DOWN khi trên

    // --- TDS ---
    float tds_target    = 300.0f;  // ppm mục tiêu
    float tds_tolerance = 50.0f;   // ±ppm (chỉ cảnh báo, không điều khiển relay)

    // --- PID gains (cho pH controller) ---
    float pid_kp = 1.0f;
    float pid_ki = 0.1f;
    float pid_kd = 0.05f;

    // ---- Helpers ----

    // Nhiệt độ mục tiêu = midpoint của [temp_min, temp_max]
    float tempTarget() const {
        return (temp_min + temp_max) * 0.5f;
    }

    // Deadband = 1/4 khoảng [temp_min, temp_max]
    // Hysteresis sẽ dùng: ON khi đi qua target ± deadband
    float tempDeadband() const {
        return (temp_max - temp_min) * 0.25f;
    }

    // pH setpoint = midpoint của [ph_min, ph_max]
    // PID controller nhắm vào giá trị này
    float phSetpoint() const {
        return (ph_min + ph_max) * 0.5f;
    }
};

// ----------------------------------------------------------------
// CONFIG VALIDATOR
// ----------------------------------------------------------------
class ConfigValidator {
public:
    // Kiểm tra toàn bộ ControlConfig
    // Trả về ConfigError::OK nếu hợp lệ, ngược lại trả lỗi đầu tiên gặp
    static ConfigError validate(const ControlConfig& cfg);

    // Trả về mô tả lỗi dạng chuỗi (tiếng Anh ngắn gọn)
    static const char* errorString(ConfigError err);
};
