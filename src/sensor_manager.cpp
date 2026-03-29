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
static unsigned long _tempRequestMs  = 0;   // millis() khi gửi requestTemperatures()
static bool          _tempRequested  = false; // đang chờ DS18B20 convert
static float         _lastRawTemp    = NAN;
static bool          _initialized    = false;

// DS18B20 conversion time tối đa ở 12-bit = 750ms
static constexpr unsigned long DS18B20_CONVERSION_MS = 800UL; // 50ms margin

// ----------------------------------------------------------------
// Calibration runtime — mặc định từ system_constants.h,
// được ghi đè bởi sensorManagerSetCalibration() khi Firebase update
// ----------------------------------------------------------------
static float _phSlope   = PH_CALIB_SLOPE_DEFAULT;
static float _phOffset  = PH_CALIB_OFFSET_DEFAULT;
static float _tdsFactor = TDS_CALIB_FACTOR_DEFAULT;

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
    return _phSlope * voltage + _phOffset;
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

    return (tdsRaw / compensation) * _tdsFactor;
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
    // DS18B20 — async mode: requestTemperatures() không block
    dsSensor.begin();
    dsSensor.setWaitForConversion(false);  // NON-BLOCKING
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

    // ── Bước A: Gửi lệnh request sớm 800ms trước chu kỳ đọc ─────
    // requestTemperatures() non-blocking, trả về ngay lập tức
    if (!_tempRequested &&
        now - _lastReadMs >= (SENSOR_READ_INTERVAL_MS - DS18B20_CONVERSION_MS)) {
        dsSensor.requestTemperatures();
        _tempRequestMs = now;
        _tempRequested = true;
    }

    // ── Bước B: Chưa đến chu kỳ 5s → thoát sớm ─────────────────
    if (now - _lastReadMs < SENSOR_READ_INTERVAL_MS) {
        return false;
    }
    _lastReadMs    = now;
    _tempRequested = false;  // reset cho chu kỳ tiếp

    // ── Bước C: Đọc kết quả (conversion đã xong từ ~800ms trước) ─
    float temp = NAN;
    float t = dsSensor.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C || isnan(t)) {
        LOG_WARNING("SENSOR", "DS18B20 read failed");
    } else if (t < DS18B20_MIN_C || t > DS18B20_MAX_C) {
        LOG_WARNING("SENSOR", "DS18B20 out of range: %.2f", t);
    } else {
        temp = t;
        _lastRawTemp = t;
    }

    float ph  = _readPh();
    delay(20); 
    float tds = _readTds(isnan(temp) ? _lastRawTemp : temp);

    SensorReading reading;
    reading.timestamp   = now;
    reading.temperature = temp;
    reading.ph          = ph;
    reading.tds         = tds;

    rawSensorBuffer.push(reading);

    LOG_DEBUG("SENSOR", "Raw T=%.2f pH=%.3f TDS=%.1f ts=%lu",
              temp, ph, tds, now);

    return true;
}

// ----------------------------------------------------------------
bool isSensorDataReady() {
    return !rawSensorBuffer.isEmpty();
}

// ----------------------------------------------------------------
// Cập nhật calibration runtime — gọi từ ConfigManager khi Firebase
// gửi settings/calibration mới. Có hiệu lực ngay chu kỳ đọc tiếp.
// ----------------------------------------------------------------
void sensorManagerSetCalibration(float phSlope, float phOffset, float tdsFactor) {
    if (phSlope == 0.0f || tdsFactor <= 0.0f) {
        LOG_ERROR("SENSOR", "setCalibration rejected: slope=%.4f factor=%.4f",
                  phSlope, tdsFactor);
        return;
    }
    _phSlope   = phSlope;
    _phOffset  = phOffset;
    _tdsFactor = tdsFactor;
    LOG_INFO("SENSOR", "Calibration updated: ph_slope=%.4f ph_offset=%.4f tds_factor=%.4f",
             _phSlope, _phOffset, _tdsFactor);
}