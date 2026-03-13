#pragma once

// ================================================================
// CRITICAL: Phải define ENABLE_DATABASE TRƯỚC khi include FirebaseClient.h
// Không có define này → RealtimeDatabase.h sẽ không được include
// (xem FirebaseClient.h line 25: #if defined(ENABLE_DATABASE))
// ================================================================
#define ENABLE_DATABASE
#define ENABLE_LEGACY_TOKEN

#include "type_definitions.h"
#include "analytics.h"
#include "safety_core.h"
#include "water_change_manager.h"
#include <Arduino.h>

// ================================================================
// wifi_firebase.h — Intelligent Aquarium v4.0
//
// WiFiManager  : kết nối WiFi non-blocking + auto-reconnect
// AquaFirebaseClient: upload telemetry 5s + 2 SSE streams
//
// Thư viện: mobizt/FirebaseClient (v2.x)
//   platformio.ini:
//     lib_deps = mobizt/FirebaseClient
//
// ── SCHEMA ──────────────────────────────────────────────────────
//  /devices/{DEVICE_ID}/
//    telemetry/    ← ESP32 ghi 5s
//    analytics/    ← ESP32 ghi 5s
//    relay_state/  ← ESP32 ghi 5s
//    status/       ← ESP32 ghi 5s (safe_mode, last_safety_event)
//    water_change/ ← ESP32 ghi state/last_run/trigger_source
//                    Web ghi manual_trigger=true
//    settings/     ← Web ghi, ESP32 stream (1 stream)
//
// ── 2 STREAMS ───────────────────────────────────────────────────
//  Stream1: /settings        → parse 5 config groups
//  Stream2: /manual_trigger  → kích hoạt thay nước từ Web
//
// ── TRIGGER SOURCE ───────────────────────────────────────────────
//  Nút vật lý  : firebaseClient.notifyButtonTrigger()
//  Lịch tự động: firebaseClient.notifyScheduleTrigger()
//  Web          : tự động qua Stream2
//  Xong         : firebaseClient.notifyTriggerDone()
// ================================================================

// ----------------------------------------------------------------
// TIMING CONSTANTS
// ----------------------------------------------------------------
#define WIFI_RECONNECT_INTERVAL_MS    5000UL
#define FIREBASE_UPLOAD_INTERVAL_MS   5000UL
#define FIREBASE_STREAM_RETRY_MS     60000UL  // stream tự reconnect, chỉ restart nếu stale >60s

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
// (Đổi tên từ FirebaseClient để tránh trùng firebase_ns::FirebaseClient)
// ================================================================
class AquaFirebaseClient {
public:
    AquaFirebaseClient();

    // Gọi từ SystemManager sau khi WiFi kết nối lần đầu
    void begin();

    // Gọi mỗi loop()
    void loop(
        const CleanReading&    clean,
        const AnalyticsResult& analytics,
        const RelayCommand&    relayState,
        WaterChangeState       wcState,
        uint32_t               wcLastRun,
        SafetyEvent            lastSafetyEvent,
        bool                   safeMode
    );

    // Push ngay lập tức (không chờ 5s)
    void pushSafetyEvent(SafetyEvent evt);

    // Trigger source — gọi từ bên ngoài
    void notifyButtonTrigger();    // main.cpp: nút vật lý → "BUTTON"
    void notifyScheduleTrigger();  // WaterChangeManager: lịch → "SCHEDULE"
    void notifyTriggerDone();      // Khi water change xong → "NONE"

    bool isReady() const { return _ready; }

    // ── Semi-public: chỉ để free-function callback truy cập ──────
    // Không gọi trực tiếp từ bên ngoài
    uint32_t _lastSettingsStreamMs;
    uint32_t _lastTriggerStreamMs;
    void _onSettingsPayload(const char* json);
    void _onTriggerPayload (bool triggered);

private:
    bool     _ready;
    uint32_t _lastUploadMs;

    // Upload
    void _uploadAll(
        const CleanReading& c, const AnalyticsResult& a,
        const RelayCommand& r, WaterChangeState wcState,
        uint32_t wcLastRun, SafetyEvent lastEvt, bool safeMode);

    String _buildTelemetryJson  (const CleanReading&    c);
    String _buildAnalyticsJson  (const AnalyticsResult& a);
    String _buildRelayJson      (const RelayCommand&    cmd);
    String _buildStatusJson     (SafetyEvent e, bool safeMode);

    // Stream management
    void _startSettingsStream();
    void _startTriggerStream();
    void _checkStreamHealth();

    // Trigger source (private)
    void _notifyWebTrigger();
    void _writeTriggerSource(const char* source);

    // Enum → string
    static const char* _wcStateStr      (WaterChangeState s);
    static const char* _safetyEventStr  (SafetyEvent      e);
    static const char* _dataSourceStr   (DataSource       s);
    static const char* _fieldStatusStr  (FieldStatus      s);
    static const char* _driftDirStr     (DriftDir         d);
};

// ----------------------------------------------------------------
// Global singletons — dùng trong main.cpp, system_manager.cpp
// ----------------------------------------------------------------
extern WiFiManager         wifiManager;
extern AquaFirebaseClient  firebaseClient;