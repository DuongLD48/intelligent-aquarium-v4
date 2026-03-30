#include "analytics.h"
#include "logger.h"
#include <math.h>
#include <string.h>

// ================================================================
// analytics.cpp
// Intelligent Aquarium v4.0
//
// EMA  → xu hướng trơn, cập nhật mỗi chu kỳ
// CUSUM → phát hiện drift dài hạn (tăng/giảm liên tục)
// WSI  → đo độ ổn định tổng hợp từ CV của 3 sensor
// FSI  → đo mức độ biến động tức thời
// ================================================================

// Global singleton
Analytics analytics;

// ----------------------------------------------------------------
Analytics::Analytics()
    : _prevTemp(25.0f),
      _hasPrev(false), _shockCount(0)
{}

// ----------------------------------------------------------------
void Analytics::reset() {
    _ema          = EmaState{};
    _cusumTemp    = CusumState{};
    _cusumTds     = CusumState{};
    _result       = AnalyticsResult{};
    _hasPrev      = false;
    _shockCount   = 0;
    LOG_INFO("ANALYTICS", "Reset");
}

void Analytics::setConfig(const AnalyticsConfig& cfg) {
    _cfg = cfg;
    LOG_INFO("ANALYTICS", "Config updated: alpha=%.2f cusumK=%.2f cusumThr=%.2f",
             cfg.ema_alpha, cfg.cusum_k, cfg.cusum_threshold);
}

// ================================================================
// UPDATE — entry point chính
// ================================================================
void Analytics::update(const CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE>& cleanBuf) {
    if (cleanBuf.isEmpty()) return;

    const CleanReading& latest = cleanBuf.last();

    _updateEma(latest);
    _updateCusum(latest);
    _updateWsi(cleanBuf);
    _updateFsi(latest);

    // Copy EMA vào result
    _result.ema_temp = _ema.temp;
    _result.ema_tds  = _ema.tds;

    LOG_VERBOSE("ANALYTICS", "WSI=%.1f FSI=%.2f drift=[%d,%d]",
                _result.wsi, _result.fsi,
                (int)_result.drift_temp,
                (int)_result.drift_tds);
}

// ================================================================
// EMA — Exponential Moving Average
// Chỉ update khi source != FALLBACK_DEFAULT
// ema_new = α × value + (1 − α) × ema_old
// ================================================================
void Analytics::_updateEma(const CleanReading& latest) {
    float alpha = _cfg.ema_alpha;

    // Khởi tạo EMA từ giá trị đầu tiên hợp lệ
    if (!_ema.initialized) {
        if (latest.source_temperature != DataSource::FALLBACK_DEFAULT &&
            latest.source_tds         != DataSource::FALLBACK_DEFAULT) {
            _ema.temp        = latest.temperature;
            _ema.tds         = latest.tds;
            _ema.initialized = true;
            LOG_DEBUG("ANALYTICS", "EMA initialized: T=%.2f TDS=%.1f",
                      _ema.temp, _ema.tds);
        }
        return;
    }

    if (latest.source_temperature != DataSource::FALLBACK_DEFAULT) {
        _ema.temp = alpha * latest.temperature + (1.0f - alpha) * _ema.temp;
    }

    if (latest.source_tds != DataSource::FALLBACK_DEFAULT) {
        _ema.tds = alpha * latest.tds + (1.0f - alpha) * _ema.tds;
    }
}

// ================================================================
// CUSUM — Cumulative Sum (drift detection)
// Chỉ chạy trên MEASURED data
//
//   S_up   = max(0, S_up   + (value − ema − k))
//   S_down = max(0, S_down + (ema − value − k))
//   S > threshold → DRIFT, reset S về 0
// ================================================================
DriftDir Analytics::_computeCusum(float value, float ema, CusumState& state) {
    float k         = _cfg.cusum_k;
    float threshold = _cfg.cusum_threshold;

    state.s_up   = fmaxf(0.0f, state.s_up   + (value - ema - k));
    state.s_down = fmaxf(0.0f, state.s_down + (ema - value - k));

    if (state.s_up >= threshold) {
        state.s_up  = 0.0f;
        state.drift = DriftDir::UP;
        return DriftDir::UP;
    }

    if (state.s_down >= threshold) {
        state.s_down = 0.0f;
        state.drift  = DriftDir::DOWN;
        return DriftDir::DOWN;
    }

    state.drift = DriftDir::NONE;
    return DriftDir::NONE;
}

void Analytics::_updateCusum(const CleanReading& latest) {
    if (!_ema.initialized) return;

    // Chỉ chạy CUSUM trên MEASURED data
    if (latest.source_temperature == DataSource::MEASURED) {
        _result.drift_temp = _computeCusum(latest.temperature, _ema.temp, _cusumTemp);
        if (_result.drift_temp != DriftDir::NONE) {
            LOG_WARNING("ANALYTICS", "Temp DRIFT %s: T=%.2f ema=%.2f",
                        _result.drift_temp == DriftDir::UP ? "UP" : "DOWN",
                        latest.temperature, _ema.temp);
        }
    }

    if (latest.source_tds == DataSource::MEASURED) {
        _result.drift_tds = _computeCusum(latest.tds, _ema.tds, _cusumTds);
        if (_result.drift_tds != DriftDir::NONE) {
            LOG_WARNING("ANALYTICS", "TDS DRIFT %s: TDS=%.1f ema=%.1f",
                        _result.drift_tds == DriftDir::UP ? "UP" : "DOWN",
                        latest.tds, _ema.tds);
        }
    }
}

// ================================================================
// WSI — Water Stability Index
//
// CV (Coefficient of Variation) = std / |mean| cho mỗi field
// trong window cuối cùng (mặc định 12 mẫu = 60 giây).
//
// WSI = 100 − (w_T × CV_T + w_pH × CV_pH + w_TDS × CV_TDS) × 100
// Clamp [0, 100]
// ================================================================
float Analytics::_coeffOfVariation(
    const CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE>& buf,
    size_t window,
    float CleanReading::*field
) const {
    size_t n = buf.size();
    if (n < 2) return 0.0f;
    if (n > window) n = window;  // Chỉ dùng window mẫu gần nhất

    size_t startIdx = buf.size() - n;  // Index đầu trong window

    // Tính mean — chỉ dùng MEASURED data
    float sum   = 0.0f;
    size_t count = 0;
    for (size_t i = startIdx; i < buf.size(); i++) {
        const CleanReading& r = buf[i];
        // Kiểm tra source: chỉ tính MEASURED
        bool isMeasured = false;
        if (field == &CleanReading::temperature)
            isMeasured = (r.source_temperature == DataSource::MEASURED);
        else if (field == &CleanReading::tds)
            isMeasured = (r.source_tds == DataSource::MEASURED);

        if (isMeasured) {
            sum += r.*field;
            count++;
        }
    }

    if (count < 2) return 0.0f;

    float mean = sum / (float)count;
    if (fabsf(mean) < 1e-6f) return 0.0f;  // Tránh chia 0

    // Tính std
    float sumSq = 0.0f;
    size_t cnt2 = 0;
    for (size_t i = startIdx; i < buf.size(); i++) {
        const CleanReading& r = buf[i];
        bool isMeasured = false;
        if (field == &CleanReading::temperature)
            isMeasured = (r.source_temperature == DataSource::MEASURED);
        else if (field == &CleanReading::tds)
            isMeasured = (r.source_tds == DataSource::MEASURED);

        if (isMeasured) {
            float diff = r.*field - mean;
            sumSq += diff * diff;
            cnt2++;
        }
    }

    float stddev = sqrtf(sumSq / (float)(cnt2 - 1));  // Sample std
    return stddev / fabsf(mean);  // CV
}

void Analytics::_updateWsi(const CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE>& buf) {
    if (buf.size() < 2) {
        _result.wsi = 100.0f;
        return;
    }

    float cv_temp = _coeffOfVariation(buf, _cfg.wsi_window, &CleanReading::temperature);
    float cv_tds  = _coeffOfVariation(buf, _cfg.wsi_window, &CleanReading::tds);

    // WSI = 100 − weighted CV sum × 100  (temp=0.6, tds=0.4)
    float instability = (_cfg.wsi_weight_temp * cv_temp +
                         _cfg.wsi_weight_tds  * cv_tds) * 100.0f;

    _result.wsi = 100.0f - instability;

    // Clamp [0, 100]
    if (_result.wsi < 0.0f)   _result.wsi = 0.0f;
    if (_result.wsi > 100.0f) _result.wsi = 100.0f;
}

// ================================================================
// FSI — Fluctuation Severity Index
//
// FSI = α×|ΔT| + penalty × (số shock trong chu kỳ này)
// So sánh CleanReading hiện tại với lần trước.
// ================================================================
void Analytics::_updateFsi(const CleanReading& latest) {
    // Đếm shock trong reading này (chỉ còn shock_temperature)
    uint32_t shocks = (latest.shock_temperature ? 1 : 0);

    if (!_hasPrev) {
        _prevTemp  = latest.temperature;
        _hasPrev   = true;
        _result.fsi = shocks * _cfg.fsi_shock_penalty;
        return;
    }

    float deltaT = fabsf(latest.temperature - _prevTemp);

    _result.fsi = _cfg.fsi_alpha * deltaT
                + _cfg.fsi_shock_penalty * (float)shocks;

    // Clamp tối thiểu 0
    if (_result.fsi < 0.0f) _result.fsi = 0.0f;

    // Cập nhật prev chỉ với MEASURED data
    if (latest.source_temperature == DataSource::MEASURED) _prevTemp = latest.temperature;

    if (_result.fsi > 5.0f) {
        LOG_DEBUG("ANALYTICS", "FSI=%.2f dT=%.3f shocks=%lu",
                  _result.fsi, deltaT, (unsigned long)shocks);
    }
}
