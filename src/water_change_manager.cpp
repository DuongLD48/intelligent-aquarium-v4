#include "water_change_manager.h"
#include "config_manager.h"
#include "system_manager.h"
#include "ph_session_manager.h"
#include "system_constants.h"
#include "logger.h"
#include <time.h>
#include <Arduino.h>

// ================================================================
// water_change_manager.cpp
// Intelligent Aquarium v4.0
//
// State machine:
//   IDLE -> PUMPING_OUT -> PUMPING_IN -> DONE -> IDLE
//
// Safety rule:
//   - Do not start while safe mode is active or while a pH session is running.
//   - If safe mode/pH session appears during water change, pause timers until
//     the system can run safely again.
// ================================================================

WaterChangeManager waterChangeManager;

WaterChangeManager::WaterChangeManager()
    : _state(WaterChangeState::IDLE),
      _stateStartMs(0),
      _lastRunDay(0),
      _lastRunTs(0),
      _manualTrigger(false),
      _schedulePending(false),
      _pausedForSafety(false),
      _pauseStartedMs(0) {}

void WaterChangeManager::begin() {
    _state           = WaterChangeState::IDLE;
    _stateStartMs    = 0;
    _lastRunDay      = 0;
    _lastRunTs       = 0;
    _manualTrigger   = false;
    _schedulePending = false;
    _pausedForSafety = false;
    _pauseStartedMs  = 0;
    LOG_INFO("WATER", "WaterChangeManager init: out=%ds in=%ds schedule=%s %02d:%02d",
             _cfg.pump_out_sec, _cfg.pump_in_sec,
             _cfg.schedule_enabled ? "ON" : "OFF",
             _cfg.schedule_hour, _cfg.schedule_minute);
}

void WaterChangeManager::restoreLastRun(uint32_t savedDay, uint32_t savedTs) {
    if (savedDay > 0) {
        _lastRunDay = savedDay;
        _lastRunTs  = savedTs;
        LOG_INFO("WATER", "Restored lastRunDay=%lu lastRunTs=%lu (from NVS)",
                 (unsigned long)_lastRunDay, (unsigned long)_lastRunTs);
    }
}

void WaterChangeManager::setConfig(const WaterChangeConfig& cfg) {
    if (cfg.schedule_hour > 23 || cfg.schedule_minute > 59) {
        LOG_ERROR("WATER", "Invalid schedule time %02d:%02d - config rejected",
                  cfg.schedule_hour, cfg.schedule_minute);
        return;
    }
    if (cfg.pump_out_sec < WATER_CHANGE_MIN_PUMP_SEC ||
        cfg.pump_in_sec  < WATER_CHANGE_MIN_PUMP_SEC) {
        LOG_ERROR("WATER", "pump time < hard floor %ds - config rejected (out=%d in=%d)",
                  WATER_CHANGE_MIN_PUMP_SEC, cfg.pump_out_sec, cfg.pump_in_sec);
        return;
    }
    if (cfg.pump_out_sec > WATER_CHANGE_MAX_PUMP_OUT_SEC ||
        cfg.pump_in_sec  > WATER_CHANGE_MAX_PUMP_IN_SEC) {
        LOG_ERROR("WATER", "pump time exceeds hard ceiling (out=%d/%d in=%d/%d) - config rejected",
                  cfg.pump_out_sec, WATER_CHANGE_MAX_PUMP_OUT_SEC,
                  cfg.pump_in_sec, WATER_CHANGE_MAX_PUMP_IN_SEC);
        return;
    }

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

void WaterChangeManager::triggerManual() {
    if (isBusy()) {
        LOG_WARNING("WATER", "triggerManual ignored: already busy (state=%s)",
                    _stateName(_state));
        return;
    }

    _manualTrigger = true;
    if (_canRunNow()) {
        LOG_INFO("WATER", "Manual trigger set");
    } else {
        LOG_WARNING("WATER", "Manual trigger queued: waiting for safe mode / pH session to clear");
    }
}

bool WaterChangeManager::isBusy() const {
    return (_state != WaterChangeState::IDLE &&
            _state != WaterChangeState::DONE);
}

void WaterChangeManager::update() {
    _tick();
}

bool WaterChangeManager::_canRunNow() const {
    return !systemManager.isSafeMode() &&
           phSessionMgr.state() == PhSessionState::IDLE;
}

void WaterChangeManager::_startWaterChange(bool scheduled) {
    _setState(WaterChangeState::PUMPING_OUT);
    _stateStartMs    = millis();
    _pausedForSafety = false;
    _pauseStartedMs  = 0;

    if (scheduled) {
        _schedulePending = false;
        _lastRunDay = _todayDay();
        LOG_INFO("WATER", "Schedule trigger -> PUMPING_OUT (%ds)", _cfg.pump_out_sec);
    } else {
        _manualTrigger = false;
        LOG_INFO("WATER", "Manual trigger -> PUMPING_OUT (%ds)", _cfg.pump_out_sec);
    }
}

void WaterChangeManager::_tick() {
    unsigned long now = millis();
    bool canRun = _canRunNow();

    if (_state == WaterChangeState::IDLE) {
        if (_cfg.schedule_enabled && _isScheduleTime()) {
            if (canRun) {
                _startWaterChange(true);
            } else if (!_schedulePending) {
                _schedulePending = true;
                LOG_WARNING("WATER", "Schedule matched but blocked by safe mode / pH session -> pending");
            }
            return;
        }

        if (_schedulePending) {
            if (canRun) {
                _startWaterChange(true);
            }
            return;
        }

        if (_manualTrigger) {
            if (canRun) {
                _startWaterChange(false);
            }
            return;
        }
    }

    if (isBusy()) {
        if (!canRun) {
            if (!_pausedForSafety) {
                _pausedForSafety = true;
                _pauseStartedMs  = now;
                LOG_WARNING("WATER", "Paused by safe mode / pH session (state=%s)",
                            _stateName(_state));
            }
            return;
        }

        if (_pausedForSafety) {
            _stateStartMs += (now - _pauseStartedMs);
            _pausedForSafety = false;
            _pauseStartedMs  = 0;
            LOG_INFO("WATER", "Resumed after safe mode / pH session cleared (state=%s)",
                     _stateName(_state));
        }
    }

    switch (_state) {
        case WaterChangeState::IDLE:
            break;

        case WaterChangeState::PUMPING_OUT:
            if ((now - _stateStartMs) >= (uint32_t)_cfg.pump_out_sec * 1000UL) {
                _setState(WaterChangeState::PUMPING_IN);
                _stateStartMs = millis();
                LOG_INFO("WATER", "PUMPING_OUT done -> PUMPING_IN (%ds)", _cfg.pump_in_sec);
            }
            break;

        case WaterChangeState::PUMPING_IN:
            if ((now - _stateStartMs) >= (uint32_t)_cfg.pump_in_sec * 1000UL) {
                _lastRunTs = (uint32_t)time(nullptr);
                if (_lastRunDay == 0 || _lastRunDay != _todayDay()) {
                    _lastRunDay = _todayDay();
                }
                _setState(WaterChangeState::DONE);
                LOG_INFO("WATER", "PUMPING_IN done -> DONE (lastRunDay=%lu ts=%lu)",
                         (unsigned long)_lastRunDay, (unsigned long)_lastRunTs);
            }
            break;

        case WaterChangeState::DONE:
        {
            WaterChangeSchedule sched = configManager.getWaterSchedule();
            sched.last_run_day = _lastRunDay;
            sched.last_run_ts  = _lastRunTs;
            configManager.saveWaterSchedule(sched);
            _setState(WaterChangeState::IDLE);
            LOG_INFO("WATER", "Water change complete -> IDLE (persisted to NVS)");
            break;
        }
    }
}

bool WaterChangeManager::_isScheduleTime() const {
    time_t now_epoch = time(nullptr);
    if (now_epoch < 1700000000L) return false;

    time_t local_epoch = now_epoch + NTP_GMT_OFFSET_SEC;
    struct tm t;
    gmtime_r(&local_epoch, &t);

    if ((uint8_t)t.tm_hour != _cfg.schedule_hour ||
        (uint8_t)t.tm_min  != _cfg.schedule_minute) {
        return false;
    }

    if (_lastRunDay == _todayDay()) return false;

    LOG_INFO("WATER", "_isScheduleTime: MATCH %02d:%02d (epoch=%lu)",
             (int)t.tm_hour, (int)t.tm_min, (unsigned long)now_epoch);
    return true;
}

uint32_t WaterChangeManager::_todayDay() const {
    time_t now_epoch = time(nullptr);
    if (now_epoch < 1700000000L) return 0;

    time_t local_epoch = now_epoch + NTP_GMT_OFFSET_SEC;
    struct tm t;
    gmtime_r(&local_epoch, &t);
    return (uint32_t)((local_epoch - t.tm_sec - t.tm_min * 60 - t.tm_hour * 3600) / 86400L);
}

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

void WaterChangeManager::_setState(WaterChangeState newState) {
    LOG_DEBUG("WATER", "State: %s -> %s",
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
