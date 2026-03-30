#pragma once
#include "type_definitions.h"
#include "control_config.h"
#include <stdint.h>

// ================================================================
// ph_dose_controller.h
// Intelligent Aquarium v4.0
//
// Thay thế PidController cho pH khi đo thưa (5 phút/lần).
//
// Thuật toán: Linear Dose — pulse tỉ lệ tuyến tính với độ lệch:
//
//   overshoot = |pH_measured - ph_boundary|
//   pulse_ms  = base_pulse_ms + pulse_per_unit × overshoot
//   pulse_ms  = clamp(pulse_ms, base_pulse_ms, max_pulse_ms)
//
// Ví dụ (base=300ms, slope=1000ms/unit, ph_min=6.5, ph_max=7.5):
//   pH = 7.6  → overshoot=0.1 → pulse = 300 + 100  = 400ms (DOWN)
//   pH = 7.9  → overshoot=0.4 → pulse = 300 + 400  = 700ms (DOWN)
//   pH = 6.3  → overshoot=0.2 → pulse = 300 + 200  = 500ms (UP)
//   pH = 7.2  → trong vùng   → không action
//
// Chỉ gọi compute() SAU khi có giá trị pH mới từ session đo.
// KHÔNG gọi mỗi 5 giây như PID cũ.
// ================================================================

// ----------------------------------------------------------------
// PH DOSE CONFIG — config riêng, lưu NVS namespace "aq_phdose"
// ----------------------------------------------------------------
struct PhDoseConfig {
    // Pulse cơ bản khi vừa ra khỏi vùng an toàn (overshoot → 0)
    uint32_t base_pulse_ms   = 300;    // ms [50, 3000]

    // Thêm bao nhiêu ms mỗi 1.0 đơn vị pH lệch khỏi biên
    uint32_t pulse_per_unit  = 1000;   // ms/unit [100, 5000]

    // Giới hạn pulse tối đa (bảo vệ, không vượt safety limit)
    uint32_t max_pulse_ms    = 3000;   // ms [100, 5000]

    // Khoảng thời gian giữa 2 session đo pH (configurable từ web)
    uint32_t measure_interval_ms = 5UL * 60UL * 1000UL;  // 5 phút default

    // Thời gian 1 session đo (bao gồm cả warm-up)
    uint32_t session_duration_ms = 60UL * 1000UL;         // 60 giây

    // Thời gian warm-up bỏ qua ở đầu session
    uint32_t warmup_ms           = 30UL * 1000UL;         // 30 giây đầu bỏ

    // Ngưỡng nhiễu: max-min của 6 mẫu > noise_threshold → NOISY
    float noise_threshold = 0.5f;   // đơn vị pH  [0.1, 2.0]

    // Ngưỡng shock: |median(N) - median(N-1)| > shock_threshold → SHOCK
    float shock_threshold = 0.5f;   // đơn vị pH  [0.1, 2.0]

    // Validate
    bool isValid() const {
        return base_pulse_ms   >= 50  && base_pulse_ms   <= 3000
            && pulse_per_unit  >= 100 && pulse_per_unit  <= 5000
            && max_pulse_ms    >= 100 && max_pulse_ms    <= 5000
            && base_pulse_ms   <= max_pulse_ms
            && warmup_ms       <  session_duration_ms
            && measure_interval_ms >= session_duration_ms
            && measure_interval_ms >= 60000UL
            && noise_threshold >= 0.1f && noise_threshold <= 2.0f
            && shock_threshold >= 0.1f && shock_threshold <= 2.0f;
    }
};

// ----------------------------------------------------------------
// PH DOSE RESULT — output của compute()
// ----------------------------------------------------------------
struct PhDoseResult {
    bool     ph_up        = false;
    bool     ph_down      = false;
    uint32_t pulse_ms     = 0;
    float    overshoot    = 0.0f;  // Độ lệch so với biên (debug)
};

// ----------------------------------------------------------------
// PH DOSE CONTROLLER
// ----------------------------------------------------------------
class PhDoseController {
public:
    PhDoseController();

    // Cập nhật config (ph_min, ph_max từ ControlConfig)
    void setControlConfig(const ControlConfig& ctrl);

    // Cập nhật PhDoseConfig runtime
    void setDoseConfig(const PhDoseConfig& cfg);
    const PhDoseConfig& getDoseConfig() const { return _cfg; }

    // Tính lệnh relay dựa trên giá trị pH đo được từ session.
    // Chỉ gọi sau khi session đo hoàn tất — KHÔNG gọi mỗi 5 giây.
    // ph: giá trị pH median của session (đã qua warm-up + pipeline)
    PhDoseResult compute(float ph);

    // Reset state
    void reset();

    // Getters để debug/OLED
    float     phMin()    const { return _ph_min; }
    float     phMax()    const { return _ph_max; }
    PhDoseResult lastResult() const { return _lastResult; }

private:
    PhDoseConfig _cfg;
    float        _ph_min;
    float        _ph_max;
    PhDoseResult _lastResult;
};

// Global singleton
extern PhDoseController phDoseCtrl;
