#include <Arduino.h>

// ---- Core headers ----
#include "system_constants.h"
#include "type_definitions.h"
#include "circular_buffer.h"
#include "credentials.h"

// ---- Module headers ----
#include "logger.h"
#include "sensor_manager.h"
#include "data_pipeline.h"
#include "control_config.h"
#include "config_manager.h"
#include "safety_core.h"
#include "hysteresis_controller.h"
#include "ph_dose_controller.h"      // ← Thay pid_controller.h
#include "ph_session_manager.h"      // ← Module mới
#include "water_change_manager.h"
#include "analytics.h"
#include "oled_display.h"
#include "button_manager.h"
#include "wifi_firebase.h"
#include "system_manager.h"
#include "firestore_history.h"

// ================================================================
// main.cpp
// Intelligent Aquarium v4.0
//
// setup() → systemManager.begin() + peripheral init
// loop()  → 18 bước tuần tự mỗi 5 giây
//
// Thay đổi so với v4.0 cũ:
//   - PidController → PhDoseController (linear dose)
//   - PhSessionManager quản lý toàn bộ vòng đời đo pH:
//       IDLE → SAFE_MODE_WAIT → COLLECTING → DOSING → IDLE
//   - Bước 14 (PID pH) → Bước 14 (PhSession update)
//   - pH pulse timer không còn ở main — PhSessionManager tự quản
// ================================================================

// ----------------------------------------------------------------
// GLOBAL STATE
// ----------------------------------------------------------------

// Clean reading history buffer (120 mẫu × 5s = 10 phút)
CircularBuffer<CleanReading, SENSOR_HISTORY_SIZE> cleanBuffer;

// DataPipeline — extern được tham chiếu trong wifi_firebase.cpp
DataPipeline dataPipeline;

// Trạng thái cuối cùng sau safety check
static CleanReading    gClean;
static AnalyticsResult gAnalytics;
static bool            gWifiConnected = false;
static SafetyEvent     gPrevSafetyEvt = SafetyEvent::NONE;

// ----------------------------------------------------------------
// DEBUG PRINT — in tóm tắt mỗi chu kỳ
// ----------------------------------------------------------------
static void debugPrintCycle(const CleanReading& c,
                             const AnalyticsResult& a,
                             const RelayCommand& cmd,
                             SafetyEvent evt) {

    const ControlConfig&     ctrl = configManager.getControlConfig();
    const PipelineConfig&    pipe = dataPipeline.getConfig();
    const SafetyLimits&      lim  = safetyCore.getLimits();
    const WaterChangeConfig& wc   = waterChangeManager.getConfig();

    LOG_DEBUG("START DEBUG", "------------------------------------------------------------");

    // ── NTP time ──────────────────────────────────────────────────
    time_t now_epoch = time(nullptr);
    if (now_epoch > 1700000000L) {
        struct tm ti;
        localtime_r(&now_epoch, &ti);
        LOG_DEBUG("MAIN", "NTP  : %04d-%02d-%02d %02d:%02d:%02d UTC+7 | epoch=%ld",
            ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
            ti.tm_hour, ti.tm_min, ti.tm_sec,
            (long)now_epoch);
    } else {
        LOG_DEBUG("MAIN", "NTP  : NOT SYNCED (epoch=%ld)", (long)now_epoch);
    }

    // ── pH Session State ──────────────────────────────────────────
    {
        const PhDoseResult& _d = phSessionMgr.lastDoseResult();
        LOG_DEBUG("PHSESS",
            "State=%-16s | nextIn=%lus | lastPH=%.3f overshoot=%.3f | pulse=%lums(%s)",
            phSessionMgr.stateStr(),
            (unsigned long)phSessionMgr.secondsUntilNextSession(),
            phSessionMgr.lastMedianPh(),
            _d.overshoot,
            (unsigned long)_d.pulse_ms,
            _d.ph_up ? "UP" : _d.ph_down ? "DOWN" : "NONE");
    }

    // ── WaterChange last run ──────────────────────────────────────
    uint32_t wc_ts = waterChangeManager.lastRunTs();
    if (wc_ts > 0) {
        struct tm wt;
        localtime_r((const time_t*)&wc_ts, &wt);
        LOG_DEBUG("MAIN", "WC   : state=%s | lastRun=%04d-%02d-%02d %02d:%02d | ts=%lu",
            waterChangeManager.getState() == WaterChangeState::IDLE        ? "IDLE" :
            waterChangeManager.getState() == WaterChangeState::PUMPING_OUT ? "OUT"  :
            waterChangeManager.getState() == WaterChangeState::PUMPING_IN  ? "IN"   : "DONE",
            wt.tm_year + 1900, wt.tm_mon + 1, wt.tm_mday,
            wt.tm_hour, wt.tm_min,
            (unsigned long)wc_ts);
    } else {
        LOG_DEBUG("MAIN", "WC   : state=%s | lastRun=NEVER",
            waterChangeManager.getState() == WaterChangeState::IDLE ? "IDLE" : "BUSY");
    }

    // ── Sensors & Relay ───────────────────────────────────────────
    LOG_DEBUG("MAIN",
        "T=%.2f(%c) TDS=%.1f(%c) | "
        "H=%d C=%d pHU=%d pHD=%d pIn=%d pOut=%d | "
        "WSI=%.0f FSI=%.2f | SafeEvt=%d",
        c.temperature, (c.source_temperature == DataSource::MEASURED ? 'M' : 'F'),
        c.tds,         (c.source_tds         == DataSource::MEASURED ? 'M' : 'F'),
        cmd.heater, cmd.cooler, cmd.ph_up, cmd.ph_down, cmd.pump_in, cmd.pump_out,
        a.wsi, a.fsi, (int)evt);

    // ── ControlConfig (settings/config) ──────────────────────────
    LOG_DEBUG("CFG",
        "Temp=[%.1f~%.1f] | pH=[%.2f~%.2f] | TDS=%.0f±%.0f",
        ctrl.temp_min, ctrl.temp_max,
        ctrl.ph_min,   ctrl.ph_max,
        ctrl.tds_target, ctrl.tds_tolerance);

    // ── PhDoseConfig (settings/ph_dose_config) ───────────────────
    const PhDoseConfig& dose = phDoseCtrl.getDoseConfig();
    LOG_DEBUG("DOSE",
        "interval=%lus session=%lus warmup=%lus | "
        "base=%lums slope=%lums/unit max=%lums | "
        "noise_thr=%.2f shock_thr=%.2f",
        (unsigned long)(dose.measure_interval_ms / 1000),
        (unsigned long)(dose.session_duration_ms / 1000),
        (unsigned long)(dose.warmup_ms           / 1000),
        (unsigned long)dose.base_pulse_ms,
        (unsigned long)dose.pulse_per_unit,
        (unsigned long)dose.max_pulse_ms,
        dose.noise_threshold,
        dose.shock_threshold);

    // ── PipelineConfig (settings/pipeline_config) ────────────────
    LOG_DEBUG("PIPE",
        "RangeT=[%.1f~%.1f] TDS=[%.0f~%.0f] | "
        "MAD win=%d min=%d thr=%.2f floorT=%.2f floorTDS=%.1f | "
        "Shock dT=%.1f",
        pipe.temp_min, pipe.temp_max,
        pipe.tds_min,  pipe.tds_max,
        (int)pipe.mad_window_size, (int)pipe.mad_min_samples,
        pipe.mad_threshold, pipe.mad_floor_temp, pipe.mad_floor_tds,
        pipe.shock_temp_delta);

    // ── SafetyLimits (settings/safety_limits) ────────────────────
    LOG_DEBUG("SAFE",
        "Cutoff=%.1f EmgCool=%.1f | "
        "HtrMax=%lus HtrCool=%lus | "
        "pHPulseMax=%lums pHInterval=%lums | "
        "Stale=%d",
        lim.thermal_cutoff_c, lim.temp_emergency_cool_c,
        (unsigned long)(lim.heater_max_runtime_ms    / 1000),
        (unsigned long)(lim.heater_cooldown_ms       / 1000),
        (unsigned long) lim.ph_pump_max_pulse_ms,
        (unsigned long) lim.ph_pump_min_interval_ms,
        (int)lim.stale_sensor_threshold);

    // ── AnalyticsConfig (settings/analytics_config) ──────────────
    const AnalyticsConfig& acfg = analytics.getConfig();
    LOG_DEBUG("ANA",
        "EMA_alpha=%.3f | CUSUM k=%.2f thr=%.2f | "
        "WSI w_T=%.2f w_TDS=%.2f | "
        "FSI alpha=%.2f penalty=%.1f",
        acfg.ema_alpha,
        acfg.cusum_k, acfg.cusum_threshold,
        acfg.wsi_weight_temp, acfg.wsi_weight_tds,
        acfg.fsi_alpha, acfg.fsi_shock_penalty);

    // ── WaterChangeConfig (settings/water_schedule) ───────────────
    LOG_DEBUG("WC",
        "Sched=%s %02d:%02d | PumpOut=%ds PumpIn=%ds",
        wc.schedule_enabled ? "ON" : "OFF",
        (int)wc.schedule_hour, (int)wc.schedule_minute,
        (int)wc.pump_out_sec,  (int)wc.pump_in_sec);

    // ── SensorCalibration (settings/calibration) ─────────────────
    const SensorCalibration& calib = configManager.getCalibration();
    LOG_DEBUG("CALIB",
        "pH: slope=%.4f offset=%.4f | TDS: factor=%.4f",
        calib.ph_slope, calib.ph_offset, calib.tds_factor);

    LOG_DEBUG("END DEBUG", "------------------------------------------------------------");
}

// ================================================================
// SETUP
// ================================================================
void setup() {
    // Khởi tạo toàn bộ hệ thống theo thứ tự cố định
    systemManager.begin();

    // Button manager — sau system (GPIO đã init)
    buttonManager.begin();

    // OLED — splash screen trong begin()
    oledDisplay.begin();

    // Áp dụng pipeline config từ NVS vào dataPipeline
    dataPipeline.setConfig(configManager.getPipelineConfig());

    // PhDoseController — apply config từ NVS + ControlConfig
    phDoseCtrl.setControlConfig(configManager.getControlConfig());
    // PhDoseConfig dùng default — có thể override từ Firebase sau

    // PhSessionManager — begin sau phDoseCtrl đã có config
    phSessionMgr.begin();

    // safetyCore.setBypass(true);   // test
    safetyCore.setBypass(false);     // production

    // systemManager.setSafeModeBypass(true);   // test
    systemManager.setSafeModeBypass(false);     // production

    LOG_INFO("MAIN", "setup() complete — entering loop()");
}

// ================================================================
// LOOP — 18 bước tuần tự mỗi 5 giây
// ================================================================
void loop() {
    // ── BƯỚC 1: Đọc nút bấm + điều hướng menu ──────────────────
    buttonManager.update();

    if (oledDisplay.handleButtons()) {
        waterChangeManager.triggerManual();
        firebaseClient.notifyButtonTrigger();
        LOG_INFO("MAIN", "Water change triggered via menu (SELECT confirm)");
    }

    // ── BƯỚC 2: System update (watchdog + safe mode check) ──────
    systemManager.update(gClean, gAnalytics);

    // ── BƯỚC 3: pH Session Manager update ───────────────────────
    // Quản lý toàn bộ vòng đời session đo pH:
    //   IDLE → SAFE_MODE_WAIT → COLLECTING → DOSING → IDLE
    // Pulse bơm pH được xử lý non-blocking bên trong phSessionMgr.
    // Trả true khi session vừa hoàn tất.
    {
        bool sessionDone = phSessionMgr.update();
        if (sessionDone) {
            // Session xong: log kết quả, Firebase sẽ upload trong bước 7
            const PhDoseResult& dose = phSessionMgr.lastDoseResult();
            LOG_INFO("MAIN",
                "pH session done: median=%.3f pulse=%lums(%s)",
                phSessionMgr.lastMedianPh(),
                (unsigned long)dose.pulse_ms,
                dose.ph_up   ? "UP"   :
                dose.ph_down ? "DOWN" : "NONE");
        }
    }

    // ── BƯỚC 4: Water change state machine ──────────────────────
    {
        WaterChangeState wcBefore = waterChangeManager.getState();
        waterChangeManager.update();
        WaterChangeState wcAfter  = waterChangeManager.getState();
        if (wcBefore == WaterChangeState::DONE &&
            wcAfter  == WaterChangeState::IDLE) {
            firebaseClient.notifyTriggerDone();
        }
    }

    // ── Safe mode: skip sensor + control pipeline ────────────────
    // Khi PhSessionManager đang trong SAFE_MODE_WAIT / COLLECTING,
    // systemManager.isSafeMode() == true → không điều khiển relay.
    // Temp/TDS vẫn đọc và pipeline chạy bình thường (chỉ pH bị hold).
    if (systemManager.isSafeMode()) {
        goto step_firebase;
    }

step_firebase:
    // ── BƯỚC 5: Serial config handler ───────────────────────────
    configManager.handleSerial();

    // ── BƯỚC 6: WiFi loop (reconnect) ───────────────────────────
    gWifiConnected = wifiManager.isConnected();
    wifiManager.loop();

    // ── BƯỚC 7: Firebase loop (stream + upload mỗi 5s) ──────────
    firebaseClient.loop(
        gClean,
        gAnalytics,
        safetyCore.currentState(),
        safetyCore.lastEvent(),
        systemManager.isSafeMode()
    );

    // ── BƯỚC 8: OLED render (throttle 500ms) ────────────────────
    oledDisplay.update(
        gClean,
        gAnalytics,
        safetyCore.currentState(),
        waterChangeManager.getState(),
        gWifiConnected,
        systemManager.isSafeMode()
    );

    // ── BƯỚC 9: Đọc sensor (mỗi 5 giây) ─────────────────────────
    bool newSample = readSensors();
    if (!newSample) {
        delay(10);
        return;
    }

    // Lấy raw reading mới nhất
    if (rawSensorBuffer.isEmpty()) return;
    SensorReading raw = rawSensorBuffer.last();

    // ── BƯỚC 10: Data pipeline (lọc 3 tầng) ─────────────────────
    CleanReading clean = dataPipeline.process(raw);

    // ── BƯỚC 11: Lưu lịch sử ────────────────────────────────────
    cleanBuffer.push(clean);
    gClean = clean;

    // ── BƯỚC 12: Analytics ──────────────────────────────────────
    analytics.update(cleanBuffer);
    gAnalytics = analytics.result();

    // Nếu safe mode → dừng ở đây (không điều khiển relay)
    if (systemManager.isSafeMode()) {
        debugPrintCycle(clean, gAnalytics, safetyCore.currentState(),
                        safetyCore.lastEvent());
        return;
    }

    // ── BƯỚC 13: Hysteresis controller (nhiệt độ) ───────────────
    RelayCommand cmd;
    cmd.allOff();
    hysteresisCtrl.compute(clean, cmd);

    // ── BƯỚC 14: pH — KHÔNG dùng PID nữa ────────────────────────
    // PhSessionManager tự quản lý pulse relay pH bên trong.
    // Trong chu kỳ 5 giây bình thường, pH pump KHÔNG được điều khiển
    // ở đây. Chỉ khi session DOSING mới bật bơm (trong phSessionMgr).
    // cmd.ph_up và cmd.ph_down giữ nguyên false từ allOff().

    // ── BƯỚC 15: Merge lệnh pump từ WaterChangeManager ──────────
    {
        bool pOut, pIn;
        waterChangeManager.getRelayCmd(pOut, pIn);
        cmd.pump_out = pOut;
        cmd.pump_in  = pIn;
    }

    // ── BƯỚC 16: Safety check (7 tầng tuần tự) ──────────────────
    SafetyEvent evt = safetyCore.apply(cmd, clean);

    if (evt != SafetyEvent::NONE) {
        if (evt != gPrevSafetyEvt) {
            firebaseClient.pushSafetyEvent(evt);
        }
        gPrevSafetyEvt = evt;
    }

    // ── BƯỚC 17: Ghi relay GPIO ──────────────────────────────────
    safetyCore.writeRelays(cmd);

    // ── BƯỚC 18: Debug print ─────────────────────────────────────
    debugPrintCycle(clean, gAnalytics, cmd, evt);
}