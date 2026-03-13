#include "sensor_manager.h"
#include "logger.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Arduino.h>
#include <math.h>

// ================================================================
// sensor_manager.cpp
// Intelligent Aquarium v4.0
//
// Đọc 3 cảm biến: DS18B20 (nhiệt độ), pH analog, TDS analog.
// Không có filter — chỉ đọc và push raw vào buffer.
// ================================================================

// ----------------------------------------------------------------
// Định nghĩa global buffer (extern trong .h)
// ----------------------------------------------------------------
CircularBuffer<SensorReading, SENSOR_HISTORY_SIZE> rawSensorBuffer;

// ----------------------------------------------------------------
// OneWire + DallasTemperature
// ----------------------------------------------------------------
static OneWire           oneWire(PIN_DS18B20);
static DallasTemperature dsSensor(&oneWire);

// ----------------------------------------------------------------
// Hằng số nội bộ
// ----------------------------------------------------------------
static constexpr float DS18B20_MIN_C     = -55.0f;
static constexpr float DS18B20_MAX_C     = 125.0f;
static constexpr float ADC_MAX           = 4095.0f;  // ESP32 ADC 12-bit
static constexpr float ADC_VREF          = 3.3f;     // Vref ESP32

// TDS temperature compensation reference
static constexpr float TDS_TEMP_REF_C    = 25.0f;

// ----------------------------------------------------------------
// State nội bộ
// ----------------------------------------------------------------
static unsigned long _lastReadMs     = 0;
static float         _lastRawTemp    = NAN;  // Lưu giá trị hợp lệ cuối
static bool          _initialized    = false;

// ----------------------------------------------------------------
// Helpers — chuyển đổi ADC → giá trị vật lý
// ----------------------------------------------------------------

// ADC raw → voltage (mV không cần, dùng V)
static inline float _adcToVoltage(int raw) {
    return (raw / ADC_MAX) * ADC_VREF;
}

// Voltage → pH dùng calibration slope + offset
// pH = slope × voltage + offset
static float _voltageToPh(float voltage) {
    return PH_CALIB_SLOPE * voltage + PH_CALIB_OFFSET;
}

// Voltage → TDS (ppm) với bù nhiệt độ
// Công thức chuẩn từ datasheet module TDS analog:
//   TDS = (133.42 × V³ − 255.86 × V² + 857.39 × V) × 0.5 × factor
//   Bù nhiệt: chia cho (1 + 0.02 × (T − 25))
static float _voltageToTds(float voltage, float tempC) {
    // Tránh NaN nếu nhiệt độ không hợp lệ → dùng 25°C
    float temp = isnan(tempC) ? TDS_TEMP_REF_C : tempC;

    float tdsRaw = (133.42f * voltage * voltage * voltage
                  - 255.86f * voltage * voltage
                  + 857.39f * voltage) * 0.5f;

    // Temperature compensation
    float compensation = 1.0f + 0.02f * (temp - TDS_TEMP_REF_C);
    if (compensation < 0.01f) compensation = 0.01f;  // Tránh chia 0

    return (tdsRaw / compensation) * TDS_CALIB_FACTOR;
}

// ----------------------------------------------------------------
// Đọc DS18B20
// Trả về nhiệt độ °C, hoặc NaN nếu lỗi.
// Giữ lại _lastRawTemp nếu đọc thất bại.
// ----------------------------------------------------------------
static float _readTemperature() {
    dsSensor.requestTemperatures();
    float t = dsSensor.getTempCByIndex(0);

    // Kiểm tra các trường hợp lỗi
    if (t == DEVICE_DISCONNECTED_C) {
        LOG_WARNING("SENSOR", "DS18B20 disconnected");
        return NAN;
    }
    if (isnan(t)) {
        LOG_WARNING("SENSOR", "DS18B20 returned NaN");
        return NAN;
    }
    if (t < DS18B20_MIN_C || t > DS18B20_MAX_C) {
        LOG_WARNING("SENSOR", "DS18B20 out of range: %.2f", t);
        return NAN;
    }

    _lastRawTemp = t;  // Cập nhật last valid raw
    return t;
}

// ----------------------------------------------------------------
// Đọc pH
// Trả về giá trị pH (có thể âm hoặc > 14 — pipeline sẽ lọc).
// ----------------------------------------------------------------
static float _readPh() {
    // Lấy trung bình 5 mẫu để giảm nhiễu ADC (đây không phải filter
    // logic, chỉ là đặc tính đọc ADC của ESP32)
    int32_t sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += analogRead(PIN_PH_ADC);
        delayMicroseconds(100);
    }
    float raw = (float)(sum / 5);
    float voltage = _adcToVoltage(raw);
    float ph = _voltageToPh(voltage);

    LOG_VERBOSE("SENSOR", "pH ADC=%d V=%.3f pH=%.3f", (int)(sum/5), voltage, ph);
    return ph;
}

// ----------------------------------------------------------------
// Đọc TDS (dùng nhiệt độ mới nhất để bù)
// ----------------------------------------------------------------
static float _readTds(float currentTemp) {
    int32_t sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += analogRead(PIN_TDS_ADC);
        delayMicroseconds(100);
    }
    float raw = (float)(sum / 5);
    float voltage = _adcToVoltage(raw);
    float tds = _voltageToTds(voltage, currentTemp);

    LOG_VERBOSE("SENSOR", "TDS ADC=%d V=%.3f TDS=%.1f ppm", (int)(sum/5), voltage, tds);
    return tds;
}

// ================================================================
// PUBLIC API
// ================================================================

void sensor_manager_init() {
    // DS18B20
    dsSensor.begin();
    uint8_t count = dsSensor.getDeviceCount();
    if (count == 0) {
        LOG_ERROR("SENSOR", "No DS18B20 found on GPIO %d!", PIN_DS18B20);
    } else {
        LOG_INFO("SENSOR", "DS18B20 found: %d device(s)", count);
    }

    // ADC pins — GPIO 34, 35 là input-only, không cần pinMode
    // nhưng set attenuation để đọc đúng 0–3.3V
    analogSetAttenuation(ADC_11db);   // Full range 0–3.3V trên ESP32

    _initialized = true;
    LOG_INFO("SENSOR", "Sensor manager init OK");
}

// ----------------------------------------------------------------
bool readSensors() {
    if (!_initialized) return false;

    unsigned long now = millis();
    if (now - _lastReadMs < SENSOR_READ_INTERVAL_MS) {
        return false;  // Chưa đến chu kỳ
    }
    _lastReadMs = now;

    // --- Đọc phần cứng ---
    float temp = _readTemperature();
    float ph   = _readPh();
    float tds  = _readTds(isnan(temp) ? _lastRawTemp : temp);

    // --- Tạo SensorReading và push vào buffer ---
    SensorReading reading;
    reading.timestamp   = now;
    reading.temperature = temp;
    reading.ph          = ph;
    reading.tds         = tds;

    rawSensorBuffer.push(reading);

    LOG_DEBUG("SENSOR", "Raw T=%.2f pH=%.3f TDS=%.1f ts=%lu",
              temp, ph, tds, now);

    return true;  // Có mẫu mới
}

// ----------------------------------------------------------------
bool isSensorDataReady() {
    return !rawSensorBuffer.isEmpty();
}
