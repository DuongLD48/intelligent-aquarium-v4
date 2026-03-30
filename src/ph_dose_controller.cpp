#include "ph_dose_controller.h"
#include "logger.h"
#include <math.h>

// ================================================================
// ph_dose_controller.cpp
// Intelligent Aquarium v4.0
//
// Linear Dose Controller cho pH — thay thế PID khi đo thưa.
// Chỉ được gọi sau mỗi session đo pH (mặc định 5 phút/lần).
// ================================================================

// Global singleton
PhDoseController phDoseCtrl;

// ----------------------------------------------------------------
PhDoseController::PhDoseController()
    : _ph_min(6.5f), _ph_max(7.5f)
{}

// ----------------------------------------------------------------
void PhDoseController::setControlConfig(const ControlConfig& ctrl) {
    _ph_min = ctrl.ph_min;
    _ph_max = ctrl.ph_max;
    LOG_INFO("DOSE", "ControlConfig updated: ph=[%.2f ~ %.2f]", _ph_min, _ph_max);
}

// ----------------------------------------------------------------
void PhDoseController::setDoseConfig(const PhDoseConfig& cfg) {
    if (!cfg.isValid()) {
        LOG_ERROR("DOSE", "setDoseConfig rejected: invalid values");
        return;
    }
    _cfg = cfg;
    LOG_INFO("DOSE", "DoseConfig: base=%lums/unit slope=%lums max=%lums interval=%lums warmup=%lums",
             (unsigned long)cfg.base_pulse_ms,
             (unsigned long)cfg.pulse_per_unit,
             (unsigned long)cfg.max_pulse_ms,
             (unsigned long)cfg.measure_interval_ms,
             (unsigned long)cfg.warmup_ms);
}

// ----------------------------------------------------------------
void PhDoseController::reset() {
    _lastResult = PhDoseResult{};
    LOG_INFO("DOSE", "Reset");
}

// ================================================================
// COMPUTE — Linear Dose
//
//   Nếu pH trong [ph_min, ph_max] → không action (deadzone)
//
//   Nếu pH > ph_max:
//     overshoot = pH - ph_max
//     pulse_ms  = base_pulse_ms + pulse_per_unit × overshoot
//     → bật ph_down
//
//   Nếu pH < ph_min:
//     overshoot = ph_min - pH
//     pulse_ms  = base_pulse_ms + pulse_per_unit × overshoot
//     → bật ph_up
//
//   Clamp: pulse_ms ∈ [base_pulse_ms, max_pulse_ms]
// ================================================================
PhDoseResult PhDoseController::compute(float ph) {
    PhDoseResult result;

    // Kiểm tra NaN / giá trị không hợp lệ
    if (isnan(ph) || ph < 0.0f || ph > 14.0f) {
        LOG_WARNING("DOSE", "compute: invalid pH=%.3f → no action", ph);
        _lastResult = result;
        return result;
    }

    // ── Deadzone: pH trong vùng an toàn ──────────────────────────
    if (ph >= _ph_min && ph <= _ph_max) {
        LOG_INFO("DOSE", "pH=%.3f in [%.2f, %.2f] → deadzone, no action",
                 ph, _ph_min, _ph_max);
        _lastResult = result;
        return result;
    }

    // ── Tính overshoot và hướng ───────────────────────────────────
    float overshoot = 0.0f;
    if (ph > _ph_max) {
        overshoot      = ph - _ph_max;
        result.ph_down = true;
        result.ph_up   = false;
    } else {
        overshoot      = _ph_min - ph;
        result.ph_up   = true;
        result.ph_down = false;
    }
    result.overshoot = overshoot;

    // ── Tính pulse_ms (tuyến tính) ────────────────────────────────
    // pulse = base + slope × overshoot
    float pulse_f = (float)_cfg.base_pulse_ms
                  + (float)_cfg.pulse_per_unit * overshoot;

    // Clamp về [base_pulse_ms, max_pulse_ms]
    if (pulse_f < (float)_cfg.base_pulse_ms) pulse_f = (float)_cfg.base_pulse_ms;
    if (pulse_f > (float)_cfg.max_pulse_ms)  pulse_f = (float)_cfg.max_pulse_ms;

    result.pulse_ms = (uint32_t)pulse_f;

    LOG_INFO("DOSE",
             "pH=%.3f %s boundary=%.2f overshoot=%.3f → %s pulse=%lums",
             ph,
             result.ph_down ? ">" : "<",
             result.ph_down ? _ph_max : _ph_min,
             overshoot,
             result.ph_up ? "pH_UP" : "pH_DOWN",
             (unsigned long)result.pulse_ms);

    _lastResult = result;
    return result;
}
