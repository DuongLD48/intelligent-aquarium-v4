#include "water_change_manager.h"
#include "config_manager.h"
#include "system_constants.h"
#include "logger.h"
#include <time.h>
#include <Arduino.h>

// ================================================================
// water_change_manager.cpp
// Intelligent Aquarium v4.0 — MODULE MỚI
//
// State machine:
//   IDLE ──trigger──→ PUMPING_OUT ──Xs──→ PUMPING_IN ──Ys──→ DONE ──→ IDLE
// ================================================================

// Global singleton
WaterChangeManager waterChangeManager;

// ----------------------------------------------------------------
// Constructor
// ----------------------------------------------------------------
WaterChangeManager::WaterChangeManager()
    : _state(WaterChangeState::IDLE),
      _stateStartMs(0),
      _lastRunDay(0),
      _lastRunTs(0),
      _manualTrigger(false)
{}

// ----------------------------------------------------------------
void WaterChangeManager::begin() {
    _state         = WaterChangeState::IDLE;
    _stateStartMs  = 0;
    _lastRunDay    = 0;
    _lastRunTs     = 0;
    _manualTrigger = false;
    LOG_INFO("WATER", "WaterChangeManager init: out=%ds in=%ds schedule=%s %02d:%02d",
             _cfg.pump_out_sec, _cfg.pump_in_sec,
             _cfg.schedule_enabled ? "ON" : "OFF",
             _cfg.schedule_hour, _cfg.schedule_minute);
}

// ----------------------------------------------------------------
// RESTORE LAST RUN — gọi từ system_manager::begin() sau begin()
// Khôi phục _lastRunDay và _lastRunTs từ NVS để tránh trigger
// lại trong ngày đã chạy sau khi reboot.
// ----------------------------------------------------------------
void WaterChangeManager::restoreLastRun(uint32_t savedDay, uint32_t savedTs) {
    if (savedDay > 0) {
        _lastRunDay = savedDay;
        _lastRunTs  = savedTs;
        LOG_INFO("WATER", "Restored lastRunDay=%lu lastRunTs=%lu (from NVS)",
                 (unsigned long)_lastRunDay, (unsigned long)_lastRunTs);
    }
}

// ----------------------------------------------------------------
void WaterChangeManager::setConfig(const WaterChangeConfig& cfg) {
    // Validate — dùng compile-time constants làm hard floor/ceiling tuyệt đối.
    // Runtime limits (pump_min_sec / pump_out_max_sec / pump_in_max_sec) đã được
    // configManager validate và enforce trước khi gọi setConfig().
    if (cfg.schedule_hour > 23 || cfg.schedule_minute > 59) {
        LOG_ERROR("WATER", "Invalid schedule time %02d:%02d — config rejected",
                  cfg.schedule_hour, cfg.schedule_minute);
        return;
    }
    if (cfg.pump_out_sec < WATER_CHANGE_MIN_PUMP_SEC ||
        cfg.pump_in_sec  < WATER_CHANGE_MIN_PUMP_SEC) {
        LOG_ERROR("WATER", "pump time < hard floor %ds — config rejected (out=%d in=%d)",
                  WATER_CHANGE_MIN_PUMP_SEC, cfg.pump_out_sec, cfg.pump_in_sec);
        return;
    }
    if (cfg.pump_out_sec > WATER_CHANGE_MAX_PUMP_OUT_SEC ||
        cfg.pump_in_sec  > WATER_CHANGE_MAX_PUMP_IN_SEC) {
        LOG_ERROR("WATER", "pump time exceeds hard ceiling (out=%d/%d in=%d/%d) — config rejected",
                  cfg.pump_out_sec, WATER_CHANGE_MAX_PUMP_OUT_SEC,
                  cfg.pump_in_sec,  WATER_CHANGE_MAX_PUMP_IN_SEC);
        return;
    }

    // Không thay config khi đang bận (tránh race condition)
    if (isBusy()) {
        LOG_WARNING("WATER", "setConfig ignored: currently busy (state=%s)",
                    _stateName(_state));
        return;
    }

    _cfg = cfg;
    LOG_INFO("WATER", "Config updated: schedule=%s %02d:%02d out=%ds in=%ds",
             cfg.schedule_enabled ? "ON" : "OFF",
             cfg.schedule_hour, cfg.schedule_minute,
             cfg.pump_out_sec, cfg.pump_in_sec);
}

// ----------------------------------------------------------------
void WaterChangeManager::triggerManual() {
    if (isBusy()) {
        LOG_WARNING("WATER", "triggerManual ignored: already busy (state=%s)",
                    _stateName(_state));
        return;
    }
    _manualTrigger = true;
    LOG_INFO("WATER", "Manual trigger set");
}

// ----------------------------------------------------------------
bool WaterChangeManager::isBusy() const {
    return (_state != WaterChangeState::IDLE &&
            _state != WaterChangeState::DONE);
}

// ================================================================
// UPDATE — gọi mỗi loop()
// ================================================================
void WaterChangeManager::update() {
    _tick();
}

// ================================================================
// TICK — state machine logic
// ================================================================
void WaterChangeManager::_tick() {
    unsigned long now = millis();

    switch (_state) {

        // --------------------------------------------------------
        case WaterChangeState::IDLE:
            // Ưu tiên 1: manual trigger (không cần NTP)
            if (_manualTrigger) {
                _manualTrigger = false;
                _setState(WaterChangeState::PUMPING_OUT);
                _stateStartMs = millis();
                LOG_INFO("WATER", "Manual trigger → PUMPING_OUT (%ds)", _cfg.pump_out_sec);
                break;
            }
            // Ưu tiên 2: lịch tự động
            if (_cfg.schedule_enabled && _isScheduleTime()) {
                // FIX: Set _lastRunDay ngay khi bắt đầu (không chờ PUMPING_IN xong)
                // Đảm bảo nếu reboot giữa chừng sẽ không trigger lại trong ngày hôm nay
                _lastRunDay = _todayDay();
                _setState(WaterChangeState::PUMPING_OUT);
                _stateStartMs = millis();
                LOG_INFO("WATER", "Schedule trigger → PUMPING_OUT (%ds)", _cfg.pump_out_sec);
            }
            break;

        // --------------------------------------------------------
        case WaterChangeState::PUMPING_OUT:
            // Relay pump_out ON được set bởi getRelayCmd()
            if ((now - _stateStartMs) >= (uint32_t)_cfg.pump_out_sec * 1000UL) {
                // Hết thời gian bơm ra → chuyển sang bơm vào
                _setState(WaterChangeState::PUMPING_IN);
                _stateStartMs = millis();
                LOG_INFO("WATER", "PUMPING_OUT done → PUMPING_IN (%ds)", _cfg.pump_in_sec);
            }
            break;

        // --------------------------------------------------------
        case WaterChangeState::PUMPING_IN:
            // Relay pump_in ON được set bởi getRelayCmd()
            if ((now - _stateStartMs) >= (uint32_t)_cfg.pump_in_sec * 1000UL) {
                // Ghi timestamp đầy đủ để Firebase/web hiển thị ngày giờ phút
                _lastRunTs  = (uint32_t)time(nullptr);
                // _lastRunDay: đã set khi bắt đầu (schedule) hoặc set tại đây (manual)
                if (_lastRunDay == 0 || _lastRunDay != _todayDay()) {
                    _lastRunDay = _todayDay();
                }
                _setState(WaterChangeState::DONE);
                LOG_INFO("WATER", "PUMPING_IN done → DONE (lastRunDay=%lu ts=%lu)",
                         (unsigned long)_lastRunDay, (unsigned long)_lastRunTs);
            }
            break;

        // --------------------------------------------------------
        case WaterChangeState::DONE:
            // Lưu last_run vào NVS ngay lập tức để reboot sau đó không chạy lại
            {
                WaterChangeSchedule sched = configManager.getWaterSchedule();
                sched.last_run_day = _lastRunDay;
                sched.last_run_ts  = _lastRunTs;
                configManager.saveWaterSchedule(sched);
            }
            // Chuyển về IDLE ngay lập tức ở tick tiếp theo
            _setState(WaterChangeState::IDLE);
            LOG_INFO("WATER", "Water change complete ✓ → IDLE (persisted to NVS)");
            break;
    }
}

// ================================================================
// IS SCHEDULE TIME
// Kiểm tra:
//   1. NTP đã sync chưa (time() > 0)
//   2. Giờ:phút hiện tại (UTC+7) khớp schedule
//   3. Chưa chạy hôm nay (_lastRunDay != hôm nay)
// ================================================================
bool WaterChangeManager::_isScheduleTime() const {
    time_t now_epoch = time(nullptr);
    if (now_epoch < 1700000000L) return false;  // NTP chưa sync

    // Áp dụng offset UTC+7 thủ công vào epoch trước khi dùng gmtime_r.
    // KHÔNG dùng localtime_r() vì configTime() trên ESP32-Arduino không
    // đảm bảo timezone được set đúng cho localtime() — có thể trả UTC thuần.
    time_t local_epoch = now_epoch + NTP_GMT_OFFSET_SEC;
    struct tm t;
    gmtime_r(&local_epoch, &t);

    // Khớp giờ:phút — window = toàn bộ phút đó (0–59 giây)
    if ((uint8_t)t.tm_hour != _cfg.schedule_hour ||
        (uint8_t)t.tm_min  != _cfg.schedule_minute) {
        return false;
    }

    // Chưa chạy hôm nay
    if (_lastRunDay == _todayDay()) return false;

    LOG_INFO("WATER", "_isScheduleTime: MATCH %02d:%02d (epoch=%lu)",
             (int)t.tm_hour, (int)t.tm_min, (unsigned long)now_epoch);
    return true;
}

// ================================================================
// TODAY DAY — epoch / 86400 (UTC+7)
// ================================================================
uint32_t WaterChangeManager::_todayDay() const {
    time_t now_epoch = time(nullptr);
    if (now_epoch < 1700000000L) return 0;
    // Áp dụng offset UTC+7 thủ công, nhất quán với _isScheduleTime()
    time_t local_epoch = now_epoch + NTP_GMT_OFFSET_SEC;
    struct tm t;
    gmtime_r(&local_epoch, &t);
    // Set về đầu ngày local rồi chia 86400 để ra số ngày tuyệt đối
    return (uint32_t)((local_epoch - t.tm_sec - t.tm_min * 60 - t.tm_hour * 3600) / 86400L);
}

// ================================================================
// GET RELAY CMD — trả lệnh pump theo state hiện tại
// ================================================================
void WaterChangeManager::getRelayCmd(bool& pump_out, bool& pump_in) const {
    switch (_state) {
        case WaterChangeState::PUMPING_OUT:
            pump_out = true;
            pump_in  = false;
            break;
        case WaterChangeState::PUMPING_IN:
            pump_out = false;
            pump_in  = true;
            break;
        default:
            pump_out = false;
            pump_in  = false;
            break;
    }
}

// ================================================================
// GET STATUS JSON — cho Firebase upload
// ================================================================
String WaterChangeManager::getStatusJson() const {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"last_run_day\":%lu,\"last_run_ts\":%lu,\"busy\":%s}",
             _stateName(_state),
             (unsigned long)_lastRunDay,
             (unsigned long)_lastRunTs,
             isBusy() ? "true" : "false");
    return String(buf);
}

// ================================================================
// HELPERS
// ================================================================
void WaterChangeManager::_setState(WaterChangeState newState) {
    LOG_DEBUG("WATER", "State: %s → %s",
              _stateName(_state), _stateName(newState));
    _state = newState;
}

const char* WaterChangeManager::_stateName(WaterChangeState s) {
    switch (s) {
        case WaterChangeState::IDLE:        return "IDLE";
        case WaterChangeState::PUMPING_OUT: return "PUMPING_OUT";
        case WaterChangeState::PUMPING_IN:  return "PUMPING_IN";
        case WaterChangeState::DONE:        return "DONE";
        default:                            return "UNKNOWN";
    }
}