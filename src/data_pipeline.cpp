#include "data_pipeline.h"
#include "logger.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ================================================================
// data_pipeline.cpp
// Intelligent Aquarium v4.0
//
// 3 tầng filter cho nhiệt độ, pH, TDS:
//   Tầng 1 — Range Gate   : loại NaN / ngoài range
//   Tầng 2 — MAD Filter   : loại spike thống kê
//   Tầng 3 — Shock Flag   : cảnh báo thay đổi đột ngột
// ================================================================

// ----------------------------------------------------------------
// Constructor — khởi tạo state
// ----------------------------------------------------------------
DataPipeline::DataPipeline()
    : _lastGoodTemp(25.0f), _lastGoodPh(7.0f), _lastGoodTds(200.0f),
      _hasLastGoodTemp(false), _hasLastGoodPh(false), _hasLastGoodTds(false),
      _fallbackTemp(0), _fallbackPh(0), _fallbackTds(0),
      _prevCleanTemp(25.0f), _prevCleanPh(7.0f),
      _hasPrevCleanTemp(false), _hasPrevCleanPh(false)
{}

// ----------------------------------------------------------------
void DataPipeline::reset() {
    _validTemp.clear();
    _validPh.clear();
    _validTds.clear();

    _hasLastGoodTemp = false;
    _hasLastGoodPh   = false;
    _hasLastGoodTds  = false;

    _fallbackTemp = 0;
    _fallbackPh   = 0;
    _fallbackTds  = 0;

    _hasPrevCleanTemp = false;
    _hasPrevCleanPh   = false;

    LOG_INFO("PIPELINE", "Pipeline reset");
}

// ----------------------------------------------------------------
void DataPipeline::setConfig(const PipelineConfig& cfg) {
    _cfg = cfg;
    LOG_INFO("PIPELINE", "Config updated: T[%.1f,%.1f] pH[%.1f,%.1f] TDS[%.0f,%.0f]",
             cfg.temp_min, cfg.temp_max,
             cfg.ph_min,   cfg.ph_max,
             cfg.tds_min,  cfg.tds_max);
}

// ================================================================
// MEDIAN HELPER
// Sắp xếp bản sao của buffer → lấy phần tử giữa
// Không sửa buffer gốc.
// ================================================================
float DataPipeline::_median(const CircularBuffer<float, MAX_WINDOW>& buf) const {
    size_t n = buf.size();
    if (n == 0) return 0.0f;

    // Copy sang mảng tạm
    float tmp[MAX_WINDOW];
    for (size_t i = 0; i < n; i++) {
        tmp[i] = buf[i];
    }

    // Insertion sort — nhanh với n nhỏ (≤ 30)
    for (size_t i = 1; i < n; i++) {
        float key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }

    // Phần tử giữa
    if (n % 2 == 1) {
        return tmp[n / 2];
    } else {
        return (tmp[n / 2 - 1] + tmp[n / 2]) * 0.5f;
    }
}

// ================================================================
// _processField — xử lý 1 field qua 3 tầng
// ================================================================
DataPipeline::FieldResult DataPipeline::_processField(
    float raw,
    float rangeMin,
    float rangeMax,
    float madFloor,
    CircularBuffer<float, MAX_WINDOW>& validBuf,
    float& lastGood,
    bool&  hasLastGood,
    uint8_t& fallbackCount
) {
    FieldResult result;

    // ============================================================
    // TẦNG 1 — RANGE GATE
    // ============================================================
    bool rangeOk = !isnan(raw) && (raw >= rangeMin) && (raw <= rangeMax);

    if (!rangeOk) {
        // Xác định lý do lỗi
        if (isnan(raw)) {
            result.status = FieldStatus::SENSOR_ERROR;
        } else {
            result.status = FieldStatus::OUT_OF_RANGE;
        }

        // Fallback: dùng last_good hoặc default
        if (hasLastGood) {
            result.value  = lastGood;
            result.source = DataSource::FALLBACK_LAST;
        } else {
            // Chưa có last_good → dùng midpoint của range làm default
            result.value  = (rangeMin + rangeMax) * 0.5f;
            result.source = DataSource::FALLBACK_DEFAULT;
        }

        fallbackCount = (fallbackCount < 255) ? fallbackCount + 1 : 255;
        result.fallback_count = fallbackCount;

        // KHÔNG push vào validBuf — giữ buffer sạch
        return result;
    }

    // ============================================================
    // TẦNG 2 — MAD FILTER
    // Tính MAD dựa trên validBuf hiện tại (chưa push raw mới)
    // Chỉ push raw vào validBuf nếu pass MAD → buffer luôn sạch
    // ============================================================
    size_t n = validBuf.size();

    if (n < _cfg.mad_min_samples) {
        // Chưa đủ mẫu → bypass MAD, accept và push luôn
        validBuf.push(raw);
        result.value        = raw;
        result.source       = DataSource::MEASURED;
        result.status       = FieldStatus::OK;
        fallbackCount       = 0;
        result.fallback_count = 0;

        lastGood    = raw;
        hasLastGood = true;
        return result;
    }

    // Tính median của validBuf hiện tại (chưa có raw mới)
    float med = _median(validBuf);

    // Tính MAD = median(|xi - median|)
    float tmp[MAX_WINDOW];
    for (size_t i = 0; i < n; i++) {
        tmp[i] = fabsf(validBuf[i] - med);
    }
    // Insertion sort để tính median của deviations
    for (size_t i = 1; i < n; i++) {
        float key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    float madVal = (n % 2 == 1) ? tmp[n / 2] : (tmp[n/2-1] + tmp[n/2]) * 0.5f;

    // Áp sàn MAD để tránh chia rất nhỏ
    if (madVal < madFloor) madVal = madFloor;

    // Z-score MAD (Rousseeuw & Croux, hệ số 0.6745)
    float z = 0.6745f * fabsf(raw - med) / madVal;

    LOG_VERBOSE("PIPELINE", "MAD n=%d med=%.3f MAD=%.4f z=%.3f",
                (int)n, med, madVal, z);

    if (z > _cfg.mad_threshold) {
        fallbackCount = (fallbackCount < 255) ? fallbackCount + 1 : 255;

        // MAD lock detection: fallback quá lâu → thực tế đã thay đổi thật sự
        // Clear toàn bộ buffer cũ rồi push raw mới → median reset về giá trị thực
        // (giữ buffer cũ là bug: median vẫn ~30.7 nên chu kỳ tiếp lại bị reject)
        if (fallbackCount >= _cfg.mad_min_samples) {
            validBuf.clear();
            validBuf.push(raw);
            result.value  = raw;
            result.source = DataSource::MEASURED;
            result.status = FieldStatus::OK;
            fallbackCount = 0;
            result.fallback_count = 0;
            lastGood    = raw;
            hasLastGood = true;
            LOG_WARNING("PIPELINE", "MAD lock → force accept %.3f, buffer CLEARED (was median=%.3f)",
                        raw, med);
            return result;
        }

        // OUTLIER — dùng median của validBuf làm fallback
        result.value  = med;   // ← median của validRawBuffer, KHÔNG phải cleanBuffer
        result.source = DataSource::FALLBACK_MEDIAN;
        result.status = FieldStatus::MAD_OUTLIER;
        result.fallback_count = fallbackCount;

        LOG_DEBUG("PIPELINE", "MAD outlier: raw=%.3f z=%.3f > %.3f → fallback median=%.3f",
                  raw, z, _cfg.mad_threshold, med);
    } else {
        // ACCEPT — push vào validBuf để dùng cho các chu kỳ sau
        validBuf.push(raw);
        result.value  = raw;
        result.source = DataSource::MEASURED;
        result.status = FieldStatus::OK;

        fallbackCount         = 0;
        result.fallback_count = 0;

        lastGood    = raw;
        hasLastGood = true;
    }

    return result;
}

// ================================================================
// process() — entry point chính
// ================================================================
CleanReading DataPipeline::process(const SensorReading& raw) {
    CleanReading out;
    out.timestamp = raw.timestamp;

    // Reset shock flags + before values
    out.shock_temperature = false;
    out.shock_ph          = false;
    out.shock_temp_before = 0.0f;
    out.shock_ph_before   = 0.0f;

    // ---- Xử lý Temperature ----
    FieldResult rTemp = _processField(
        raw.temperature,
        _cfg.temp_min, _cfg.temp_max,
        _cfg.mad_floor_temp,
        _validTemp,
        _lastGoodTemp, _hasLastGoodTemp,
        _fallbackTemp
    );
    out.temperature         = rTemp.value;
    out.source_temperature  = rTemp.source;
    out.status_temperature  = rTemp.status;
    out.fallback_count_temp = rTemp.fallback_count;

    // ---- Xử lý pH ----
    FieldResult rPh = _processField(
        raw.ph,
        _cfg.ph_min, _cfg.ph_max,
        _cfg.mad_floor_ph,
        _validPh,
        _lastGoodPh, _hasLastGoodPh,
        _fallbackPh
    );
    out.ph         = rPh.value;
    out.source_ph  = rPh.source;
    out.status_ph  = rPh.status;
    out.fallback_count_ph = rPh.fallback_count;

    // ---- Xử lý TDS ----
    FieldResult rTds = _processField(
        raw.tds,
        _cfg.tds_min, _cfg.tds_max,
        _cfg.mad_floor_tds,
        _validTds,
        _lastGoodTds, _hasLastGoodTds,
        _fallbackTds
    );
    out.tds        = rTds.value;
    out.source_tds = rTds.source;
    out.status_tds = rTds.status;
    out.fallback_count_tds = rTds.fallback_count;

    // ============================================================
    // TẦNG 3 — SHOCK FLAG
    // Chỉ check khi field vừa MEASURED (không phải fallback)
    // So sánh với giá trị clean trước đó
    // ============================================================

    // Shock nhiệt độ
    if (rTemp.source == DataSource::MEASURED && _hasPrevCleanTemp) {
        float delta = fabsf(out.temperature - _prevCleanTemp);
        if (delta > _cfg.shock_temp_delta) {
            out.shock_temperature = true;
            out.shock_temp_before = _prevCleanTemp;  // giá trị trước shock
            LOG_WARNING("PIPELINE", "Shock TEMP: %.2f → %.2f (delta=%.2f)",
                        _prevCleanTemp, out.temperature, delta);
        }
    }
    if (rTemp.source == DataSource::MEASURED) {
        _prevCleanTemp    = out.temperature;
        _hasPrevCleanTemp = true;
    }

    // Shock pH
    if (rPh.source == DataSource::MEASURED && _hasPrevCleanPh) {
        float delta = fabsf(out.ph - _prevCleanPh);
        if (delta > _cfg.shock_ph_delta) {
            out.shock_ph = true;
            out.shock_ph_before = _prevCleanPh;  // giá trị trước shock
            LOG_WARNING("PIPELINE", "Shock pH: %.3f → %.3f (delta=%.3f)",
                        _prevCleanPh, out.ph, delta);
        }
    }
    if (rPh.source == DataSource::MEASURED) {
        _prevCleanPh    = out.ph;
        _hasPrevCleanPh = true;
    }

    // ---- Summary log ----
    LOG_DEBUG("PIPELINE",
              "Clean T=%.2f(%s) pH=%.3f(%s) TDS=%.1f(%s) shock=[%d,%d]",
              out.temperature, out.status_temperature == FieldStatus::OK ? "OK" : "FB",
              out.ph,          out.status_ph          == FieldStatus::OK ? "OK" : "FB",
              out.tds,         out.status_tds         == FieldStatus::OK ? "OK" : "FB",
              out.shock_temperature, out.shock_ph);

    return out;
}