#pragma once

// ================================================================
// CRITICAL: Phải define ENABLE_DATABASE TRƯỚC khi include FirebaseClient.h
// ================================================================
#define ENABLE_DATABASE
#define ENABLE_LEGACY_TOKEN

#include "type_definitions.h"
#include "analytics.h"
#include "safety_core.h"
#include "water_change_manager.h"
#include <Arduino.h>

// ================================================================
// wifi_firebase.h — Intelligent Aquarium v4.2 (PhDose update)
//
// WiFiManager  : kết nối WiFi non-blocking + auto-reconnect
// AquaFirebaseClient: upload telemetry 10s + 2 SSE streams
//
// Thay đổi:
//   - Bỏ PID gains khỏi settings/config
//   + Thêm settings/ph_dose_config group (stream + parse)
//   + Upload ph_session node (state, last_median_ph, last_pulse_ms)
//
// ── SCHEMA ──────────────────────────────────────────────────────
//  /devices/{DEVICE_ID}/
//    telemetry/                  ← ESP32 ghi 10s
//    analytics/                  ← ESP32 ghi 10s
//    relay_state/                ← ESP32 ghi 10s
//    status/                     ← ESP32 ghi 10s
//    ph_session/                 ← ESP32 ghi 10s (state, median_ph, pulse...)
//    water_change/               ← ESP32 + Web
//    settings/
//      config/                   ← temp, pH range, TDS (không còn PID)
//      ph_dose_config/           ← interval, base_ms, slope, max_ms, warmup
//      pipeline_config/
//      analytics_config/
//      safety_limits/
//      water_schedule/
//      calibration/
//    history/
//      chart/{ts}
//      shock_event_ph/{ts}
//      shock_event_temp/{ts}
//      last_safety_event/{ts}
//
// ── 2 STREAMS ───────────────────────────────────────────────────
//  Stream1: /settings        → parse 7 config groups
//  Stream2: /manual_trigger  → kích hoạt thay nước
// ================================================================

// ----------------------------------------------------------------
// TIMING CONSTANTS
// ----------------------------------------------------------------
#define WIFI_RECONNECT_INTERVAL_MS    5000UL
#define FIREBASE_UPLOAD_INTERVAL_MS  10000UL
#define FIREBASE_HISTORY_INTERVAL_MS 60000UL
#define FIREBASE_STREAM_RETRY_MS     60000UL

// ----------------------------------------------------------------
// DB PATH HELPERS
// ----------------------------------------------------------------
#define DB_ROOT      "/devices/" FIREBASE_DEVICE
#define DB_PATH(sub)  DB_ROOT "/" sub

// ================================================================
// WiFiManager
// ================================================================
class WiFiManager {
public:
    WiFiManager();
    void begin(const char* ssid, const char* password);
    void loop();
    bool isConnected() const;
    int  rssi()        const;
private:
    const char* _ssid;
    const char* _password;
    uint32_t    _lastReconnectMs;
    bool        _wasConnected;
    void _connect();
};

// ================================================================
// AquaFirebaseClient
// ================================================================
class AquaFirebaseClient {
public:
    AquaFirebaseClient();

    void begin();
    void restart();

    void loop(
        const CleanReading&    clean,
        const AnalyticsResult& analytics,
        const RelayCommand&    relayState,
        SafetyEvent            lastSafetyEvent,
        bool                   safeMode
    );

    void pushSafetyEvent(SafetyEvent evt);

    // pH session events — gọi từ PhSessionManager
    void logPhSensorError(float spread, float threshold, uint8_t samples);
    void clearPhSensorErrorFlag();
    void logPhShockEvent (float phBefore, float phAfter, float delta);

    void notifyButtonTrigger();
    void notifyScheduleTrigger();
    void notifyTriggerDone();

    bool isReady() const { return _ready; }

    // Semi-public: cho free-function callbacks truy cập
    uint32_t _lastSettingsStreamMs;
    uint32_t _lastTriggerStreamMs;
    void _onSettingsPayload(const char* path, const char* data);
    void _onTriggerPayload (bool triggered);

private:
    bool     _ready;
    uint32_t _lastUploadMs;
    uint32_t _lastHistoryMs;

    // Rising edge tracking (chỉ còn temp — pH shock log từ PhSessionManager)
    bool _prevShockTemp;
    bool _phSensorError;   // true khi session NOISY, reset sau session thành công

    // History dedup — chỉ ghi field nếu giá trị thay đổi
    float _prevHistTemp;
    float _prevHistTds;
    float _prevHistPh;   // lấy từ phSessionMgr.lastMedianPh()

    void _uploadAll(
        const CleanReading& c, const AnalyticsResult& a,
        const RelayCommand& r, SafetyEvent lastEvt, bool safeMode);

    void _uploadHistory    (const CleanReading& c);
    void _uploadShockEvents(const CleanReading& c);

    String _buildTelemetryJson  (const CleanReading&    c);
    String _buildAnalyticsJson  (const AnalyticsResult& a);
    String _buildRelayJson      (const RelayCommand&    cmd);
    String _buildStatusJson     (bool safeMode);
    String _buildPhSessionJson  ();   // ← Mới: trạng thái pH session

    void _startSettingsStream();
    void _startTriggerStream();
    void _checkStreamHealth();

    void _notifyWebTrigger();
    void _writeTriggerSource(const char* source);

    static const char* _wcStateStr    (WaterChangeState s);
    static const char* _safetyEventStr(SafetyEvent      e);
    static const char* _dataSourceStr (DataSource       s);
    static const char* _fieldStatusStr(FieldStatus      s);
    static const char* _driftDirStr   (DriftDir         d);
};

// ----------------------------------------------------------------
// Global singletons
// ----------------------------------------------------------------
extern WiFiManager         wifiManager;
extern AquaFirebaseClient  firebaseClient;