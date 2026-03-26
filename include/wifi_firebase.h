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
// wifi_firebase.h — Intelligent Aquarium v4.2
//
// WiFiManager  : kết nối WiFi non-blocking + auto-reconnect
// AquaFirebaseClient: upload telemetry 10s + 2 SSE streams
//
// Thư viện: mobizt/FirebaseClient (v2.x)
//   platformio.ini:
//     lib_deps = mobizt/FirebaseClient
//
// ── SCHEMA ──────────────────────────────────────────────────────
//  /devices/{DEVICE_ID}/
//    telemetry/                  <- ESP32 ghi 10s
//    analytics/                  <- ESP32 ghi 10s
//    relay_state/                <- ESP32 ghi 10s
//    status/                     <- ESP32 ghi 10s (bỏ last_safety_event)
//    water_change/               <- ESP32 ghi state/last_run/trigger_source
//                                   Web ghi manual_trigger=true
//    settings/                   <- Web ghi, ESP32 stream
//    history/
//      chart/{ts}                <- ESP32 ghi 60s {temp, ph, tds}
//      shock_event_ph/{ts}       <- khi shock pH  {ph_before, ph_after, is_read}
//      shock_event_temp/{ts}     <- khi shock temp {temp_before, temp_after, is_read}
//      last_safety_event/{ts}    <- khi safety event {event, is_read}
//
// ── 2 STREAMS ───────────────────────────────────────────────────
//  Stream1: /settings        -> parse 5 config groups
//  Stream2: /manual_trigger  -> kich hoat thay nuoc tu Web
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

    void notifyButtonTrigger();
    void notifyScheduleTrigger();
    void notifyTriggerDone();

    bool isReady() const { return _ready; }

    // Semi-public: chi de free-function callback truy cap
    uint32_t _lastSettingsStreamMs;
    uint32_t _lastTriggerStreamMs;
    void _onSettingsPayload(const char* path, const char* data);
    void _onTriggerPayload (bool triggered);

private:
    bool     _ready;
    uint32_t _lastUploadMs;
    uint32_t _lastHistoryMs;

    // Rising edge tracking — chỉ gửi shock event khi false → true
    bool _prevShockPh;
    bool _prevShockTemp;

    void _uploadAll(
        const CleanReading& c, const AnalyticsResult& a,
        const RelayCommand& r, SafetyEvent lastEvt, bool safeMode);

    void _uploadHistory     (const CleanReading& c);
    void _uploadShockEvents (const CleanReading& c);  // ghi shock_event_ph/temp khi có shock

    String _buildTelemetryJson  (const CleanReading&    c);
    String _buildAnalyticsJson  (const AnalyticsResult& a);
    String _buildRelayJson      (const RelayCommand&    cmd);
    String _buildStatusJson     (bool safeMode);

    void _startSettingsStream();
    void _startTriggerStream();
    void _checkStreamHealth();

    void _notifyWebTrigger();
    void _writeTriggerSource(const char* source);

    static const char* _wcStateStr      (WaterChangeState s);
    static const char* _safetyEventStr  (SafetyEvent      e);
    static const char* _dataSourceStr   (DataSource       s);
    static const char* _fieldStatusStr  (FieldStatus      s);
    static const char* _driftDirStr     (DriftDir         d);
};

// ----------------------------------------------------------------
// Global singletons
// ----------------------------------------------------------------
extern WiFiManager         wifiManager;
extern AquaFirebaseClient  firebaseClient;