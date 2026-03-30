#include "sensor_manager.h"
#include "logger.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Arduino.h>
#include <math.h>

// ================================================================
// sensor_manager.cpp
// Intelligent Aquarium v4.1
//
// readSensors(): đọc Temp + TDS mỗi 5s. pH = NAN luôn.
// readPhOnce():  đọc pH 1 lần duy nhất, chỉ gọi từ PhSessionManager
//                khi đang trong phase COLLECTING (safe mode = relay tắt hết).
//
// Lý do tách biệt:
//   - Relay gây nhiễu điện từ lên ADC pH rất nặng
//   - Chỉ đọc pH khi tất cả relay đã tắt (trong safe mode của session)
//   - Loop bình thường không cần pH → không đọc
// ================================================================

// ----------------------------------------------------------------
// Global buffer
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
static constexpr float DS18B20_MIN_C          = -55.0f;
static constexpr float DS18B20_MAX_C          = 125.0f;
static constexpr float ADC_MAX                = 4095.0f;
static constexpr float ADC_VREF               = 3.3f;
static constexpr float TDS_TEMP_REF_C         = 25.0f;
static constexpr unsigned long DS18B20_CONVERSION_MS = 800UL;

// ----------------------------------------------------------------
// State nội bộ
// ----------------------------------------------------------------
static unsigned long _lastReadMs    = 0;
static unsigned long _tempRequestMs = 0;
static bool          _tempRequested = false;
static float         _lastRawTemp   = NAN;
static bool          _initialized   = false;

// ----------------------------------------------------------------
// Calibration runtime
// ----------------------------------------------------------------
static float _phSlope   = PH_CALIB_SLOPE_DEFAULT;
static float _phOffset  = PH_CALIB_OFFSET_DEFAULT;
static float _tdsFactor = TDS_CALIB_FACTOR_DEFAULT;

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
static inline float _adcToVoltage(int raw) {
    return (raw / ADC_MAX) * ADC_VREF;
}

static float _voltageToPh(float voltage) {
    return _phSlope * voltage + _phOffset;
}

static float _voltageToTds(float voltage, float tempC) {
    float temp = isnan(tempC) ? TDS_TEMP_REF_C : tempC;
    float tdsRaw = (133.42f * voltage * voltage * voltage
                  - 255.86f * voltage * voltage
                  + 857.39f * voltage) * 0.5f;
    float compensation = 1.0f + 0.02f * (temp - TDS_TEMP_REF_C);
    if (compensation < 0.01f) compensation = 0.01f;
    return (tdsRaw / compensation) * _tdsFactor;
}

// ================================================================
// INIT
// ================================================================
void sensor_manager_init() {
    dsSensor.begin();
    dsSensor.setWaitForConversion(false);
    uint8_t count = dsSensor.getDeviceCount();
    if (count == 0) {
        LOG_ERROR("SENSOR", "No DS18B20 found on GPIO %d!", PIN_DS18B20);
    } else {
        LOG_INFO("SENSOR", "DS18B20 found: %d device(s)", count);
    }
    analogSetAttenuation(ADC_11db);
    _initialized = true;
    LOG_INFO("SENSOR", "Sensor manager init OK");
}

// ================================================================
// READ SENSORS — mỗi 5 giây, Temp + TDS chỉ, pH = NAN
//
// pH KHÔNG được đọc ở đây. Lý do:
//   - Relay heater/cooler đang chạy gây nhiễu điện từ nặng lên ADC pH
//   - Khi đọc pH ở đây, giá trị dao động ±2 đơn vị mỗi chu kỳ (xem log)
//   - Giải pháp: chỉ đọc pH khi safe mode (relay tắt) qua readPhOnce()
// ================================================================
bool readSensors() {
    if (!_initialized) return false;

    unsigned long now = millis();

    // Request DS18B20 trước 800ms
    if (!_tempRequested &&
        now - _lastReadMs >= (SENSOR_READ_INTERVAL_MS - DS18B20_CONVERSION_MS)) {
        dsSensor.requestTemperatures();
        _tempRequestMs = now;
        _tempRequested = true;
    }

    if (now - _lastReadMs < SENSOR_READ_INTERVAL_MS) return false;
    _lastReadMs    = now;
    _tempRequested = false;

    // Đọc nhiệt độ
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

    // Đọc TDS
    delay(5);
    int32_t sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += analogRead(PIN_TDS_ADC);
        delayMicroseconds(100);
    }
    float tds = _voltageToTds(
        _adcToVoltage((float)(sum / 5)),
        isnan(temp) ? _lastRawTemp : temp
    );

    // pH không đọc trong loop bình thường — chỉ qua PhSessionManager (safe mode)
    SensorReading reading;
    reading.timestamp   = now;
    reading.temperature = temp;
    reading.tds         = tds;

    rawSensorBuffer.push(reading);

    LOG_DEBUG("SENSOR", "Raw T=%.2f pH=-- TDS=%.1f ts=%lu",
              temp, tds, now);

    return true;
}

// ================================================================
// READ PH ONCE — đọc 1 mẫu pH duy nhất
//
// Chỉ gọi từ PhSessionManager khi đang COLLECTING (relay tắt hết).
// Lấy trung bình 16 mẫu ADC để giảm nhiễu lượng tử.
// Không push vào rawSensorBuffer — trả về giá trị thô cho session tự xử lý.
// ================================================================
float readPhOnce() {
    // 16 mẫu ADC, delay nhỏ giữa các lần để tránh nhiễu chuyển mạch ADC
    int32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(PIN_PH_ADC);
        delayMicroseconds(200);
    }
    float voltage = _adcToVoltage((float)(sum / 16));
    float ph      = _voltageToPh(voltage);
    LOG_DEBUG("SENSOR", "readPhOnce: adc=%d V=%.4f pH=%.3f",
              (int)(sum / 16), voltage, ph);
    return ph;
}

// ================================================================
// HELPERS
// ================================================================
bool isSensorDataReady() {
    return !rawSensorBuffer.isEmpty();
}

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