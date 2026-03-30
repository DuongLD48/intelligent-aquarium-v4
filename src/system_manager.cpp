#include "system_manager.h"
#include "logger.h"
#include "safety_core.h"
#include "config_manager.h"
#include "sensor_manager.h"
#include "hysteresis_controller.h"
#include "ph_dose_controller.h"
#include "ph_session_manager.h"
#include "water_change_manager.h"
#include "analytics.h"
#include "wifi_firebase.h"
#include "system_constants.h"
#include "credentials.h"
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <Arduino.h>

// ================================================================
// system_manager.cpp
// Intelligent Aquarium v4.1
// Thay đổi: bỏ pidCtrl, thêm phDoseCtrl + phSessionMgr,
//           thêm public enterSafeMode() / exitSafeMode()
// ================================================================

SystemManager systemManager;

SystemManager::SystemManager()
    : _safeMode(false),
      _ntpSynced(false),
      _multiSensorLost(false),
      _sessionSafeMode(false),
      _highFsiCount(0),
      _lastNtpAttemptMs(0),
      _lastNtpSyncMs(0),
      _lastWifiState(false)
{}

// ================================================================
// BEGIN
// ================================================================
void SystemManager::begin() {
    // 1. Logger
    Logger::instance().init(115200);
    Logger::instance().setLevel(LogLevel::DEBUG);
    LOG_INFO("SYS", "=== Intelligent Aquarium v4.1 booting ===");

    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:  reasonStr = "POWER_ON";      break;
        case ESP_RST_SW:       reasonStr = "SOFTWARE";      break;
        case ESP_RST_PANIC:    reasonStr = "PANIC/CRASH";   break;
        case ESP_RST_INT_WDT:  reasonStr = "INT_WATCHDOG";  break;
        case ESP_RST_TASK_WDT: reasonStr = "TASK_WATCHDOG"; break;
        case ESP_RST_WDT:      reasonStr = "WATCHDOG";      break;
        case ESP_RST_DEEPSLEEP:reasonStr = "DEEP_SLEEP";    break;
        case ESP_RST_BROWNOUT: reasonStr = "BROWNOUT";      break;
        default: break;
    }
    LOG_INFO("SYS", "Reset reason: %s (%d)", reasonStr, (int)reason);

    // 2. SafetyCore
    safetyCore.begin();
    LOG_INFO("SYS", "SafetyCore OK");

    // 3. ConfigManager
    configManager.begin();
    LOG_INFO("SYS", "ConfigManager OK");

    // 4. SensorManager
    sensor_manager_init();
    LOG_INFO("SYS", "SensorManager OK");

    // 5. Controllers — hysteresis + PhDoseController (bỏ PID)
    const ControlConfig& ctrl = configManager.getControlConfig();
    hysteresisCtrl.setConfig(ctrl);
    phDoseCtrl.setControlConfig(ctrl);
    phDoseCtrl.setDoseConfig(configManager.getPhDoseConfig());
    LOG_INFO("SYS", "Controllers OK (temp=[%.1f~%.1f] pH=[%.2f~%.2f])",
             ctrl.temp_min, ctrl.temp_max, ctrl.ph_min, ctrl.ph_max);

    // 6. DataPipeline config ready (applied in main.cpp setup)
    LOG_INFO("SYS", "Pipeline config ready");

    // 7. WaterChangeManager
    {
        const WaterChangeSchedule& sched = configManager.getWaterSchedule();
        WaterChangeConfig wcfg;
        wcfg.schedule_enabled = sched.enabled;
        wcfg.schedule_hour    = sched.hour;
        wcfg.schedule_minute  = sched.minute;
        wcfg.pump_out_sec     = sched.pump_out_sec;
        wcfg.pump_in_sec      = sched.pump_in_sec;
        waterChangeManager.begin();
        waterChangeManager.restoreLastRun(sched.last_run_day, sched.last_run_ts);
        waterChangeManager.setConfig(wcfg);
    }
    LOG_INFO("SYS", "WaterChangeManager OK");

    // 8. Analytics
    analytics.reset();
    LOG_INFO("SYS", "Analytics OK");

    // 9. PhSessionManager — sau khi phDoseCtrl đã có config
    phSessionMgr.begin();
    LOG_INFO("SYS", "PhSessionManager OK");

    // 10. Watchdog 30s
    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
    LOG_INFO("SYS", "Watchdog init (%ds)", (int)WATCHDOG_TIMEOUT_S);

    // 11. WiFi
    wifiManager.begin(WIFI_SSID, WIFI_PASSWORD);
    LOG_INFO("SYS", "=== Boot complete, entering main loop ===");
}

// ================================================================
// UPDATE — gọi đầu mỗi loop()
// ================================================================
void SystemManager::update(const CleanReading& clean,
                            const AnalyticsResult& aResult) {
    // Feed watchdog
    esp_task_wdt_reset();

    uint32_t now = millis();

    // NTP sync
    if (!_ntpSynced && wifiManager.isConnected()) {
        if (now - _lastNtpAttemptMs > NTP_RETRY_INTERVAL_MS) {
            _lastNtpAttemptMs = now;
            syncNtp();
        }
    }
    // NTP re-sync mỗi 6h
    if (_ntpSynced && (now - _lastNtpSyncMs > NTP_RESYNC_INTERVAL_MS)) {
        _lastNtpSyncMs = now;
        syncNtp();
    }

    // Firebase begin sau khi WiFi kết nối lần đầu
    static bool fbStarted = false;
    if (!fbStarted && wifiManager.isConnected()) {
        firebaseClient.begin();
        fbStarted = true;
        LOG_INFO("SYS", "Firebase started after WiFi connect");
    }

    // Restart Firebase nếu heap thấp
    if (wifiManager.isConnected() && ESP.getFreeHeap() < MIN_FREE_HEAP_BYTES) {
        LOG_WARNING("SYS", "Low heap (%lu) → restart Firebase",
                    (unsigned long)ESP.getFreeHeap());
        firebaseClient.restart();
    }

    // ── Safe mode check 1: Temp mất liên tục ───────────────────
    // Session safe mode được đánh dấu _sessionSafeMode → bỏ qua check này
    if (!_sessionSafeMode) {
        uint8_t tempStaleLimit = safetyCore.getLimits().stale_sensor_threshold * 2;
        if (clean.fallback_count_temp >= tempStaleLimit) {
            _multiSensorLost = true;
            if (!_safeMode) {
                LOG_ERROR("SYS", "SAFE MODE: Temp lost for %d cycles",
                          (int)clean.fallback_count_temp);
                _enterSafeMode();
            }
            return;
        } else {
            // Temp phục hồi
            if (_multiSensorLost) {
                _multiSensorLost = false;
                if (_safeMode && !_sessionSafeMode) {
                    LOG_INFO("SYS", "Temp recovered → exit safe mode");
                    _exitSafeMode();
                }
            }
        }

        // ── Safe mode check 2: FSI cao liên tục ─────────────────
        if (aResult.fsi > SAFE_MODE_FSI_THRESHOLD) {
            _highFsiCount++;
            if (_highFsiCount >= SAFE_MODE_FSI_CYCLES && !_safeMode) {
                LOG_ERROR("SYS", "SAFE MODE: FSI=%.1f > %.0f for %d cycles",
                          aResult.fsi, SAFE_MODE_FSI_THRESHOLD,
                          (int)SAFE_MODE_FSI_CYCLES);
                _enterSafeMode();
            }
        } else {
            if (_highFsiCount > 0) _highFsiCount = 0;
            if (_safeMode && !_sessionSafeMode && !_multiSensorLost) {
                LOG_INFO("SYS", "Conditions normal → exit safe mode");
                _exitSafeMode();
            }
        }
    }
}

// ================================================================
// NTP SYNC
// ================================================================
void SystemManager::syncNtp() {
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    LOG_INFO("SYS", "NTP sync requested: %s (UTC+%d)",
             NTP_SERVER, (int)(NTP_GMT_OFFSET_SEC / 3600));
    uint32_t deadline = millis() + 5000;
    while (millis() < deadline) {
        time_t now = time(nullptr);
        if (now > 1700000000L) {
            _ntpSynced    = true;
            _lastNtpSyncMs = millis();
            struct tm ti;
            localtime_r(&now, &ti);
            LOG_INFO("SYS", "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
            return;
        }
        delay(100);
    }
    LOG_WARNING("SYS", "NTP sync timeout — will retry in %ds",
                (int)(NTP_RETRY_INTERVAL_MS / 1000));
}

// ================================================================
// SAFE MODE — private helpers
// ================================================================
void SystemManager::_enterSafeMode() {
    _safeMode = true;
    _emergencyOff();
    LOG_ERROR("SYS", "*** SAFE MODE ACTIVE ***");
}

void SystemManager::_exitSafeMode() {
    _safeMode     = false;
    _highFsiCount = 0;
    LOG_INFO("SYS", "Safe mode cleared");
}

void SystemManager::_emergencyOff() {
    RelayCommand allOff;
    allOff.allOff();
    safetyCore.writeRelays(allOff);
    LOG_WARNING("SYS", "emergencyOff: all relays forced OFF");
}

// ================================================================
// PUBLIC SAFE MODE CONTROL — dùng bởi PhSessionManager
// ================================================================

// Kích hoạt safe mode trước khi session đo pH bắt đầu.
void SystemManager::enterSafeMode() {
    _sessionSafeMode = true;
    if (!_safeMode) {
        _enterSafeMode();
        LOG_INFO("SYS", "enterSafeMode: pH session started");
    } else {
        // Đã safe mode do sensor → chỉ đánh dấu
        LOG_INFO("SYS", "enterSafeMode: already in safe mode, session flag set");
    }
}

// Thoát safe mode sau khi session đo pH xong.
// Chỉ thoát nếu không có sensor trigger đang active.
void SystemManager::exitSafeMode() {
    _sessionSafeMode = false;

    // Kiểm tra xem sensor trigger có còn active không
    bool sensorTrigger = _multiSensorLost
                      || (_highFsiCount >= SAFE_MODE_FSI_CYCLES);

    if (sensorTrigger) {
        LOG_WARNING("SYS", "exitSafeMode: sensor trigger still active — staying in safe mode");
        return;
    }

    if (_safeMode) {
        _exitSafeMode();
        LOG_INFO("SYS", "exitSafeMode: pH session done, safe mode cleared");
    }
}

// ================================================================
// SAFE MODE BYPASS (test)
// ================================================================
void SystemManager::setSafeModeBypass(bool bypass) {
    _safeModeBypass = bypass;
    LOG_INFO("SYS", "Safe mode bypass: %s", bypass ? "ON" : "OFF");
}

bool SystemManager::isSafeModeBypass() const {
    return _safeModeBypass;
}
