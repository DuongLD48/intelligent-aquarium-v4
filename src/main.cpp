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
#include "pid_controller.h"
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

// ----------------------------------------------------------------
// PH PULSE TIMER — non-blocking
// Safety Core đã kiểm tra interval, đây chỉ tắt relay sau khi hết pulse
// ----------------------------------------------------------------
static struct PhPulse {
    bool     active    = false;
    uint8_t  relayMask = 0;   // bit 0 = ph_up, bit 1 = ph_down
    uint32_t offAtMs   = 0;
} gPhPulse;

static void startPhPulse(bool phUp, bool phDown, uint32_t durationMs) {
    if (durationMs == 0) return;
    // Clamp theo safety limit
    uint32_t maxPulse = safetyCore.getLimits().ph_pump_max_pulse_ms;
    if (durationMs > maxPulse) durationMs = maxPulse;

    gPhPulse.active    = true;
    gPhPulse.relayMask = (phUp ? 1 : 0) | (phDown ? 2 : 0);
    gPhPulse.offAtMs   = millis() + durationMs;

    LOG_DEBUG("MAIN", "pH pulse started: up=%d down=%d dur=%lums",
              phUp, phDown, (unsigned long)durationMs);
}

static void tickPhPulse() {
    if (!gPhPulse.active) return;
    if (millis() >= gPhPulse.offAtMs) {
        // Hết thời gian → tắt pH pump qua GPIO trực tiếp
        if (gPhPulse.relayMask & 1) digitalWrite(PIN_RELAY_PH_UP,   HIGH); // active LOW OFF
        if (gPhPulse.relayMask & 2) digitalWrite(PIN_RELAY_PH_DOWN, HIGH);
        gPhPulse.active = false;
        LOG_DEBUG("MAIN", "pH pulse ended");
    }
}

// ----------------------------------------------------------------
// DEBUG PRINT — in tóm tắt mỗi chu kỳ (chỉ ở level VERBOSE)
// ----------------------------------------------------------------
// static void debugPrintCycle(const CleanReading& c,
//                              const AnalyticsResult& a,
//                              const RelayCommand& cmd,
//                              SafetyEvent evt) {
//     LOG_DEBUG("MAIN",
//         "T=%.2f(%c) pH=%.3f(%c) TDS=%.1f(%c) | "
//         "H=%d C=%d pHU=%d pHD=%d pIn=%d pOut=%d | "
//         "WSI=%.0f FSI=%.2f | SafeEvt=%d | WC=%s",
//         c.temperature, (c.source_temperature == DataSource::MEASURED ? 'M' : 'F'),
//         c.ph,          (c.source_ph          == DataSource::MEASURED ? 'M' : 'F'),
//         c.tds,         (c.source_tds         == DataSource::MEASURED ? 'M' : 'F'),
//         cmd.heater, cmd.cooler, cmd.ph_up, cmd.ph_down, cmd.pump_in, cmd.pump_out,
//         a.wsi, a.fsi, (int)evt,
//         waterChangeManager.getState() == WaterChangeState::IDLE ? "IDLE" :
//         waterChangeManager.getState() == WaterChangeState::PUMPING_OUT ? "OUT" :
//         waterChangeManager.getState() == WaterChangeState::PUMPING_IN  ? "IN"  : "DONE"
//     );
// }

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

    // ── Sensors & Relay (giữ nguyên) ─────────────────────────────
    LOG_DEBUG("MAIN",
        "T=%.2f(%c) pH=%.3f(%c) TDS=%.1f(%c) | "
        "H=%d C=%d pHU=%d pHD=%d pIn=%d pOut=%d | "
        "WSI=%.0f FSI=%.2f | SafeEvt=%d",
        c.temperature, (c.source_temperature == DataSource::MEASURED ? 'M' : 'F'),
        c.ph,          (c.source_ph          == DataSource::MEASURED ? 'M' : 'F'),
        c.tds,         (c.source_tds         == DataSource::MEASURED ? 'M' : 'F'),
        cmd.heater, cmd.cooler, cmd.ph_up, cmd.ph_down, cmd.pump_in, cmd.pump_out,
        a.wsi, a.fsi, (int)evt);

    // ── ControlConfig (user settings) ─────────────────────────────
    LOG_DEBUG("CFG",
        "Temp=[%.1f~%.1f] tgt=%.2f db=%.2f | "
        "pH=[%.2f~%.2f] sp=%.2f | "
        "TDS=%.0f±%.0f | "
        "PID Kp=%.2f Ki=%.2f Kd=%.2f",
        ctrl.temp_min, ctrl.temp_max, ctrl.tempTarget(), ctrl.tempDeadband(),
        ctrl.ph_min,   ctrl.ph_max,   ctrl.phSetpoint(),
        ctrl.tds_target, ctrl.tds_tolerance,
        ctrl.pid_kp, ctrl.pid_ki, ctrl.pid_kd
    );

    // ── PipelineConfig (admin — range gate + MAD + shock) ─────────
    LOG_DEBUG("PIPE",
        "RangeT=[%.1f~%.1f] pH=[%.1f~%.1f] TDS=[%.0f~%.0f] | "
        "MAD win=%d min=%d thr=%.2f floor=[%.2f %.2f %.1f] | "
        "Shock dT=%.1f dpH=%.2f",
        pipe.temp_min, pipe.temp_max,
        pipe.ph_min,   pipe.ph_max,
        pipe.tds_min,  pipe.tds_max,
        (int)pipe.mad_window_size, (int)pipe.mad_min_samples, pipe.mad_threshold,
        pipe.mad_floor_temp, pipe.mad_floor_ph, pipe.mad_floor_tds,
        pipe.shock_temp_delta, pipe.shock_ph_delta
    );

    // ── SafetyLimits (admin — ngưỡng bảo vệ) ──────────────────────
    LOG_DEBUG("SAFE",
        "Cutoff=%.1f EmgCool=%.1f | "
        "HtrMax=%lus HtrCool=%lus | "
        "pHPulse=%lums pHInterval=%lums | "
        "Stale=%d",
        lim.thermal_cutoff_c, lim.temp_emergency_cool_c,
        (unsigned long)(lim.heater_max_runtime_ms   / 1000),
        (unsigned long)(lim.heater_cooldown_ms      / 1000),
        (unsigned long)(lim.ph_pump_max_pulse_ms),
        (unsigned long)(lim.ph_pump_min_interval_ms),
        (int)lim.stale_sensor_threshold
    );

    // ── WaterChangeConfig ──────────────────────────────────────────
    LOG_DEBUG("WC",
        "Sched=%s %02d:%02d | PumpOut=%ds PumpIn=%ds",
        wc.schedule_enabled ? "ON" : "OFF",
        (int)wc.schedule_hour, (int)wc.schedule_minute,
        (int)wc.pump_out_sec,  (int)wc.pump_in_sec
    );
    // ── FirestoreHistory status ────────────────────────────────────
    {
        uint32_t secLeft = firestoreHistory.secondsUntilFlush();
        uint32_t bktMs   = firestoreHistory.getCurrentBucketMs();
        uint32_t lastTs  = firestoreHistory.getLastFlushTs();

        int bktH = (int)((bktMs / 3600000UL) % 24);
        int bktM = (int)((bktMs / 60000UL)   % 60);

        if (lastTs > 0) {
            struct tm lt;
            localtime_r((const time_t*)&lastTs, &lt);
            LOG_DEBUG("FSH",
                "ready=%s | samples=[T:%d pH:%d TDS:%d] | "
                "bucket=%02d:%02d | flush_in=%lus | "
                "flushCount=%lu | lastFlush=%02d:%02d:%02d",
                firestoreHistory.isReady() ? "YES" : "NO",
                (int)firestoreHistory.getTempCount(),
                (int)firestoreHistory.getPhCount(),
                (int)firestoreHistory.getTdsCount(),
                bktH, bktM,
                (unsigned long)secLeft,
                (unsigned long)firestoreHistory.getFlushCount(),
                lt.tm_hour, lt.tm_min, lt.tm_sec);
        } else {
            LOG_DEBUG("FSH",
                "ready=%s | samples=[T:%d pH:%d TDS:%d] | "
                "bucket=%02d:%02d | flush_in=%lus | "
                "flushCount=%lu | lastFlush=NEVER",
                firestoreHistory.isReady() ? "YES" : "NO",
                (int)firestoreHistory.getTempCount(),
                (int)firestoreHistory.getPhCount(),
                (int)firestoreHistory.getTdsCount(),
                bktH, bktM,
                (unsigned long)secLeft,
                (unsigned long)firestoreHistory.getFlushCount());
        }
    }
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

    safetyCore.setBypass(true);  // test
    // safetyCore.setBypass(false); // production  
    
    // systemManager.setSafeModeBypass(true);  // test
    systemManager.setSafeModeBypass(false); // production

    LOG_INFO("MAIN", "setup() complete — entering loop()");
}

// ================================================================
// LOOP — 18 bước tuần tự
// ================================================================
void loop() {
    // ── BƯỚC 1: Đọc nút bấm + điều hướng menu ──────────────────
    buttonManager.update();
 
    // handleButtons() xử lý UP/DOWN/SELECT/BACK theo màn hình hiện tại
    // Trả true khi user xác nhận thay nước qua menu
    if (oledDisplay.handleButtons()) {
        waterChangeManager.triggerManual();
        firebaseClient.notifyButtonTrigger();
        LOG_INFO("MAIN", "Water change triggered via menu (SELECT confirm)");
    }

    // ── BƯỚC 2: System update (watchdog + safe mode check) ──────
    systemManager.update(gClean, gAnalytics);

    // ── BƯỚC 3: pH pulse timer (tắt relay khi hết thời gian) ───
    // Chạy trước safe mode check: cần tắt relay đúng cách dù safe mode
    tickPhPulse();

    // ── BƯỚC 4: Water change state machine ──────────────────────
    // QUAN TRỌNG: Luôn chạy dù safe mode — bơm đang hoạt động cần
    // được tắt đúng cách theo timer, không để treo ở PUMPING_OUT mãi.
    {
        WaterChangeState wcBefore = waterChangeManager.getState();
        waterChangeManager.update();
        WaterChangeState wcAfter  = waterChangeManager.getState();
        // Vừa chuyển về IDLE sau khi DONE → báo Firebase trigger_source = NONE
        if (wcBefore == WaterChangeState::DONE &&
            wcAfter  == WaterChangeState::IDLE) {
            firebaseClient.notifyTriggerDone();
        }
    }

    // Nếu đang safe mode → bỏ qua bước điều khiển (13-17)
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

    // ── BƯỚC 9: Đọc sensor (mỗi 5 giây) ────────────────────────
    bool newSample = readSensors();
    if (!newSample) {
        // Chưa đến chu kỳ → yield và thoát sớm
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
    gClean = clean;  // Cập nhật global để OLED + system check dùng

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

    // ── BƯỚC 14: PID controller (pH) ─────────────────────────── 
    pidCtrl.compute(clean, cmd, SENSOR_READ_INTERVAL_MS);

    // Nếu PID tính ra pulse pH → khởi động pulse timer
    if ((cmd.ph_up || cmd.ph_down) && pidCtrl.pulseDurationMs() > 0) {
        startPhPulse(cmd.ph_up, cmd.ph_down, pidCtrl.pulseDurationMs());
    }

    // ── BƯỚC 15: Merge lệnh pump từ WaterChangeManager ──────────
    {
        bool pOut, pIn;
        waterChangeManager.getRelayCmd(pOut, pIn);
        cmd.pump_out = pOut;
        cmd.pump_in  = pIn;
    }

    // ── BƯỚC 16: Safety check (7 tầng tuần tự) ──────────────────
    SafetyEvent evt = safetyCore.apply(cmd, clean);

    // Push safety event lên Firebase nếu có
    if (evt != SafetyEvent::NONE) {
        firebaseClient.pushSafetyEvent(evt);
    }

    // ── BƯỚC 17: Ghi relay GPIO ──────────────────────────────────
    safetyCore.writeRelays(cmd);

    // ── BƯỚC 18: Debug print ─────────────────────────────────────
    debugPrintCycle(clean, gAnalytics, cmd, evt);
    
}