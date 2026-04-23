#pragma once
#include "type_definitions.h"
#include "system_constants.h"
#include <Arduino.h>

// ================================================================
// water_change_manager.h
// Intelligent Aquarium v4.0
//
// Water change flow:
//   IDLE -> PUMPING_OUT -> PUMPING_IN -> DONE -> IDLE
//
// Water change must not start while the system is in safe mode or while
// a pH measurement session is active. If safe mode appears in the middle
// of a water-change run, the timers are paused until the system becomes
// safe again.
// ================================================================

struct WaterChangeConfig {
    bool     schedule_enabled = false;
    uint8_t  schedule_hour    = 8;
    uint8_t  schedule_minute  = 0;
    uint16_t pump_out_sec     = WATER_CHANGE_DEFAULT_PUMP_OUT_SEC;
    uint16_t pump_in_sec      = WATER_CHANGE_DEFAULT_PUMP_IN_SEC;
};

class WaterChangeManager {
public:
    WaterChangeManager();

    void begin();
    void setConfig(const WaterChangeConfig& cfg);
    void update();

    // Manual trigger from UI / Firebase.
    // Ignored if already busy. If safe mode or pH session is active,
    // the trigger is kept pending until the system can run safely again.
    void triggerManual();

    bool isBusy() const;
    WaterChangeState getState() const { return _state; }

    void getRelayCmd(bool& pump_out, bool& pump_in) const;
    String getStatusJson() const;

    const WaterChangeConfig& getConfig() const { return _cfg; }
    uint32_t lastRunDay() const { return _lastRunDay; }
    uint32_t lastRunTs() const { return _lastRunTs; }

    void restoreLastRun(uint32_t savedDay, uint32_t savedTs);

private:
    WaterChangeConfig _cfg;
    WaterChangeState  _state;
    unsigned long     _stateStartMs;
    uint32_t          _lastRunDay;
    uint32_t          _lastRunTs;
    bool              _manualTrigger;
    bool              _schedulePending;
    bool              _pausedForSafety;
    unsigned long     _pauseStartedMs;

    void _tick();
    bool _canRunNow() const;
    void _startWaterChange(bool scheduled);
    bool _isScheduleTime() const;
    uint32_t _todayDay() const;
    void _setState(WaterChangeState newState);
    static const char* _stateName(WaterChangeState s);
};

extern WaterChangeManager waterChangeManager;
