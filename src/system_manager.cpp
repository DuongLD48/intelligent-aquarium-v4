#include "system_manager.h"
#include "logger.h"
#include "safety_core.h"
#include "config_manager.h"
#include "sensor_manager.h"
#include "hysteresis_controller.h"
#include "pid_controller.h"
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
// Intelligent Aquarium v4.0
// ================================================================

SystemManager systemManager;

SystemManager::SystemManager()
    : _safeMode(false), _ntpSynced(false),
      _highFsiCount(0), _lastNtpAttemptMs(0)
{}

// ================================================================
// BEGIN — khởi tạo theo thứ tự cố định
// ================================================================
void SystemManager::begin() {
    // 1. Logger — trước tiên để các bước sau có thể log
    Logger::instance().init(115200);
    Logger::instance().setLevel(LogLevel::VERBOSE);
    LOG_INFO("SYS", "=== Intelligent Aquarium v4.0 booting ===");

    // Log reset reason — quan trọng để debug crash/watchdog
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:   reasonStr = "POWER_ON";    break;
        case ESP_RST_SW:        reasonStr = "SOFTWARE";    break;
        case ESP_RST_PANIC:     reasonStr = "PANIC/CRASH"; break;
        case ESP_RST_INT_WDT:   reasonStr = "INT_WATCHDOG";break;
        case ESP_RST_TASK_WDT:  reasonStr = "TASK_WATCHDOG"; break;
        case ESP_RST_WDT:       reasonStr = "WATCHDOG";    break;
        case ESP_RST_DEEPSLEEP: reasonStr = "DEEP_SLEEP";  break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT";    break;
        case ESP_RST_SDIO:      reasonStr = "SDIO";        break;
        default: break;
    }
    LOG_INFO("SYS", "Reset reason: %s (%d)", reasonStr, (int)reason);

    // 2. SafetyCore — relay pins OUTPUT + tắt hết ngay từ đầu
    safetyCore.begin();
    LOG_INFO("SYS", "SafetyCore OK");

    // 3. ConfigManager — load NVS, chuẩn bị config
    configManager.begin();
    LOG_INFO("SYS", "ConfigManager OK");

    // 4. SensorManager — OneWire DS18B20, ADC
    sensor_manager_init();
    LOG_INFO("SYS", "SensorManager OK");

    // 5. Controllers — apply config hiện tại
    const ControlConfig& ctrl = configManager.getControlConfig();
    hysteresisCtrl.setConfig(ctrl);
    pidCtrl.setConfig(ctrl);
    LOG_INFO("SYS", "Controllers OK (target=%.2f setpoint=%.2f)",
             ctrl.tempTarget(), ctrl.phSetpoint());

    // 6. DataPipeline — apply pipeline config
    // dataPipeline là extern từ main.cpp, nhưng setConfig gọi ở đây
    // thông qua configManager (sẽ apply khi stream đến hoặc manual)
    LOG_INFO("SYS", "Pipeline config ready");

    // 7. WaterChangeManager — apply water schedule + khôi phục last_run từ NVS
    {
        const WaterChangeSchedule& sched = configManager.getWaterSchedule();
        WaterChangeConfig wcfg;
        wcfg.schedule_enabled = sched.enabled;
        wcfg.schedule_hour    = sched.hour;
        wcfg.schedule_minute  = sched.minute;
        wcfg.pump_out_sec     = sched.pump_out_sec;
        wcfg.pump_in_sec      = sched.pump_in_sec;
        waterChangeManager.begin();
        // FIX: Khôi phục last_run từ NVS trước setConfig()
        // Tránh trigger lại trong ngày đã chạy sau khi reboot
        waterChangeManager.restoreLastRun(sched.last_run_day, sched.last_run_ts);
        waterChangeManager.setConfig(wcfg);
    }
    LOG_INFO("SYS", "WaterChangeManager OK");

    // 8. Analytics — reset state
    analytics.reset();
    LOG_INFO("SYS", "Analytics OK");

    // 9. Watchdog 30s
    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
    LOG_INFO("SYS", "Watchdog init (%ds)", (int)WATCHDOG_TIMEOUT_S);

    // 10. WiFi — non-blocking
    wifiManager.begin(WIFI_SSID, WIFI_PASSWORD);
    LOG_INFO("SYS", "WiFi connecting...");

    // 11. NTP — sẽ sync sau khi WiFi kết nối (trong update())
    LOG_INFO("SYS", "=== Boot complete, entering main loop ===");
}

// ================================================================
// UPDATE — gọi đầu mỗi loop()
// ================================================================
void SystemManager::update(const CleanReading& clean,
                            const AnalyticsResult& aResult) {
    // Feed watchdog
    esp_task_wdt_reset();

    // NTP sync — thử sau khi WiFi có, retry mỗi 30s
    if (!_ntpSynced && wifiManager.isConnected()) {
        uint32_t now = millis();
        if (now - _lastNtpAttemptMs > NTP_RETRY_INTERVAL_MS) {
            _lastNtpAttemptMs = now;
            syncNtp();
        }
    }

    // Firebase begin — sau khi WiFi kết nối lần đầu
    static bool fbStarted = false;
    if (!fbStarted && wifiManager.isConnected()) {
        firebaseClient.begin();
        fbStarted = true;
        LOG_INFO("SYS", "Firebase started after WiFi connect");
    }

    // ---- Safe mode check 1: Temp mất liên tục ----
    // Nếu temp fallback_count >= stale_sensor_threshold * 2
    // (khoảng 60s với chu kỳ 5s và threshold=6 → 12 chu kỳ = 60s)
    uint8_t tempStaleLimit = safetyCore.getLimits().stale_sensor_threshold * 2;
    if (clean.fallback_count_temp >= tempStaleLimit) {
        if (!_safeMode) {
            LOG_ERROR("SYS", "SAFE MODE: Temp lost for %d cycles (>%ds)",
                      clean.fallback_count_temp,
                      (int)(tempStaleLimit * 5));
            _enterSafeMode();
        }
        return;
    }

    // ---- Safe mode check 2: FSI > 50 liên tục 5 chu kỳ ----
    if (aResult.fsi > SAFE_MODE_FSI_THRESHOLD) {
        _highFsiCount++;
        if (_highFsiCount >= SAFE_MODE_FSI_CYCLES && !_safeMode) {
            LOG_ERROR("SYS", "SAFE MODE: FSI=%.1f > %.0f for %d cycles",
                      aResult.fsi, SAFE_MODE_FSI_THRESHOLD,
                      (int)SAFE_MODE_FSI_CYCLES);
            _enterSafeMode();
        }
    } else {
        // FSI bình thường → reset counter và thoát safe mode nếu đang trong đó
        if (_highFsiCount > 0) _highFsiCount = 0;
        if (_safeMode) {
            LOG_INFO("SYS", "Conditions normal — exiting safe mode");
            _exitSafeMode();
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

    // Chờ tối đa 5s để lấy thời gian
    uint32_t deadline = millis() + 5000;
    while (millis() < deadline) {
        time_t now = time(nullptr);
        if (now > 1700000000L) {
            _ntpSynced = true;
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
// SAFE MODE
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
    // Tắt tất cả relay ngay lập tức, bypass mọi controller
    RelayCommand allOff;
    allOff.allOff();
    safetyCore.writeRelays(allOff);
    LOG_WARNING("SYS", "emergencyOff: all relays forced OFF");
}

// ================================================================
// SAFE MODE BYPASS (cho phép tắt hoàn toàn safe mode để test)
// ================================================================

void SystemManager::setSafeModeBypass(bool bypass) {
    _safeModeBypass = bypass;
    LOG_INFO("SYS", "Safe mode bypass: %s", bypass ? "ON" : "OFF");
}