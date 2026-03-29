// ================================================================
// wifi_firebase.cpp — Intelligent Aquarium v4.2
//
// Thư viện: mobizt/FirebaseClient v2.x
//
// Thay đổi so với v4.1:
//   - Bỏ FirestoreHistory (RAM + delay)
//   + History ghi thẳng vào RTDB /history/{unix_ts} mỗi 60s
//     dùng chung aClient — không tốn SSL context thêm
//
// ── ĐIỂM QUAN TRỌNG TỪ SOURCE THỰC TẾ ──────────────────────────
//
// 1. ENABLE_DATABASE phải define TRƯỚC include FirebaseClient.h
//    (đã define trong wifi_firebase.h)
//
// 2. AsyncClientClass v2.1+ là network-independent
//    → Constructor chỉ cần WiFiClientSecure, không cần DefaultNetwork
//    → AsyncClientClass aClient(ssl_client);
//
// 3. initializeApp signatures (từ source line 307, 322):
//    - initializeApp(aClient, app, getAuth(auth), timeoutMs, cb)
//    - initializeApp(aClient, app, getAuth(auth), cb, uid)
//
// 4. set<T> signatures (từ source line 439, 464):
//    - set(aClient, path, value, aResult)
//    - set(aClient, path, value, cb, uid, etag)
//
// 5. get (SSE stream) signature (từ source line 122):
//    - get(aClient, path, cb, sse=true, uid)
//
// 6. Callback type: AsyncResultCallback = void(*)(AsyncResult&)
//    → Phải là free function hoặc static, KHÔNG là lambda trong
//      một số compiler versions của Arduino/ESP32
//    → Dùng free function để an toàn nhất
//
// 7. RealtimeDatabaseResult truy cập qua:
//    aResult.to<RealtimeDatabaseResult>()
//    rồi RTDB.event(), RTDB.dataPath(), RTDB.to<T>()
// ================================================================

#include "wifi_firebase.h"
#include "credentials.h"
#include "logger.h"
#include "config_manager.h"
#include "data_pipeline.h"
#include "analytics.h"
#include "safety_core.h"
#include "water_change_manager.h"
#include "system_constants.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>    // Include SAU wifi_firebase.h

#include <time.h>
#include <string.h>
#include <stdio.h>

// ----------------------------------------------------------------
// Forward extern
// ----------------------------------------------------------------
extern DataPipeline dataPipeline;

// ----------------------------------------------------------------
// using alias
// ----------------------------------------------------------------
using AsyncClient = AsyncClientClass;

// ----------------------------------------------------------------
// Forward declarations — free function callbacks
// ----------------------------------------------------------------
static void onUploadResult (AsyncResult& r);
static void onStream1Result(AsyncResult& r);
static void onStream2Result(AsyncResult& r);

// ================================================================
// Global singletons
// ================================================================
WiFiManager         wifiManager;
AquaFirebaseClient  firebaseClient;

// ================================================================
// Firebase v2 objects — file-scope
// ================================================================

// SSL clients — mỗi connection cần 1 instance riêng
static WiFiClientSecure ssl_upload;    // cho set/patch RTDB
static WiFiClientSecure ssl_stream1;   // cho stream /settings
static WiFiClientSecure ssl_stream2;   // cho stream /manual_trigger

// AsyncClient v2.1+: constructor chỉ cần ssl client
static AsyncClient aClient      (ssl_upload);
static AsyncClient aClientStream1(ssl_stream1);
static AsyncClient aClientStream2(ssl_stream2);

// Auth: LegacyToken (Database Secret)
static LegacyToken  legacy_auth(FIREBASE_TOKEN);
static FirebaseApp  app;
static RealtimeDatabase Database;

// Con trỏ tới instance để callback tự do có thể gọi lại methods
static AquaFirebaseClient* _fbClient = nullptr;

// ================================================================
// ================================================================
// WiFiManager
// ================================================================
// ================================================================

WiFiManager::WiFiManager()
    : _ssid(nullptr), _password(nullptr),
      _lastReconnectMs(0), _wasConnected(false) {}

void WiFiManager::begin(const char* ssid, const char* password) {
    _ssid = ssid;
    _password = password;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    _connect();
}

void WiFiManager::loop() {
    bool c = isConnected();
    if (_wasConnected && !c)
        LOG_WARNING("WIFI", "Disconnected!");
    if (!_wasConnected && c)
        LOG_INFO("WIFI", "Connected! IP=%s RSSI=%d dBm",
                 WiFi.localIP().toString().c_str(), rssi());
    _wasConnected = c;

    if (!c) {
        uint32_t now = millis();
        if (now - _lastReconnectMs >= WIFI_RECONNECT_INTERVAL_MS) {
            _lastReconnectMs = now;
            LOG_INFO("WIFI", "Reconnecting \"%s\"...", _ssid);
            _connect();
        }
    }
}

bool WiFiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }
int  WiFiManager::rssi()        const { return isConnected() ? (int)WiFi.RSSI() : 0; }
void WiFiManager::_connect()          { WiFi.begin(_ssid, _password); }

// ================================================================
// ================================================================
// AquaFirebaseClient
// ================================================================
// ================================================================

AquaFirebaseClient::AquaFirebaseClient()
    : _ready(false), _lastUploadMs(0), _lastHistoryMs(0),
      _lastSettingsStreamMs(0), _lastTriggerStreamMs(0),
      _prevShockPh(false), _prevShockTemp(false) {}

// ================================================================
// RESTART — reset Firebase state sạch, dùng lại SSL objects đã có
// Gọi khi: WiFi reconnect hoặc heap < MIN_FREE_HEAP_BYTES
// ================================================================
void AquaFirebaseClient::restart() {
    LOG_WARNING("FB", "Restarting Firebase client...");
    _ready = false;

    // Re-init auth + database binding
    initializeApp(aClient, app, getAuth(legacy_auth), onUploadResult, "authRestart");
    app.getApp<RealtimeDatabase>(Database);
    Database.url(FIREBASE_URL);

    // Restart cả 2 stream
    _startSettingsStream();
    _startTriggerStream();

    _ready = true;
    uint32_t now = millis();
    _lastUploadMs         = now;
    _lastHistoryMs        = now;
    _lastSettingsStreamMs = now;
    _lastTriggerStreamMs  = now;

    LOG_INFO("FB", "Firebase restarted. heap=%lu", (unsigned long)ESP.getFreeHeap());
}

// ================================================================
// BEGIN
// ================================================================
void AquaFirebaseClient::begin() {
    if (_ready) return;
    _fbClient = this;

    LOG_INFO("FB", "Initializing...");

    // Bỏ verify SSL certificate
    ssl_upload.setInsecure();
    ssl_stream1.setInsecure();
    ssl_stream2.setInsecure();

    // initializeApp với LegacyToken + callback
    initializeApp(aClient, app, getAuth(legacy_auth), onUploadResult, "authTask");

    // Bind RealtimeDatabase với app, set URL
    app.getApp<RealtimeDatabase>(Database);
    Database.url(FIREBASE_URL);

    // Bắt đầu 2 stream
    _startSettingsStream();
    _startTriggerStream();

    _ready = true;
    uint32_t now = millis();
    _lastUploadMs         = 0;
    _lastHistoryMs        = 0;
    _lastSettingsStreamMs = now;
    _lastTriggerStreamMs  = now;

    LOG_INFO("FB", "Ready. Device=%s", FIREBASE_DEVICE);
}

// ================================================================
// LOOP
// ================================================================
void AquaFirebaseClient::loop(
    const CleanReading&    clean,
    const AnalyticsResult& analytics,
    const RelayCommand&    relayState,
    SafetyEvent            lastSafetyEvent,
    bool                   safeMode)
{
    if (!_ready || !wifiManager.isConnected()) return;

    // BẮT BUỘC gọi mỗi iteration
    app.loop();
    Database.loop();

    uint32_t now = millis();

    // Upload telemetry/analytics/relay/status/water_change mỗi 10s
    if (now - _lastUploadMs >= FIREBASE_UPLOAD_INTERVAL_MS) {
        _lastUploadMs = now;
        LOG_DEBUG("FB", "heap=%lu minHeap=%lu",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap());
        _uploadAll(clean, analytics, relayState, lastSafetyEvent, safeMode);
    }

    // Ghi history vào RTDB mỗi 60s
    if (now - _lastHistoryMs >= FIREBASE_HISTORY_INTERVAL_MS) {
        _lastHistoryMs = now;
        _uploadHistory(clean);
    }

    // Ghi shock events ngay khi phát hiện (mỗi cycle, hàm tự guard)
    _uploadShockEvents(clean);

    _checkStreamHealth();
}

// ================================================================
// UPLOAD ALL
// ================================================================
void AquaFirebaseClient::_uploadAll(
    const CleanReading& c, const AnalyticsResult& a,
    const RelayCommand& r, SafetyEvent lastEvt, bool safeMode)
{
    // 4 set riêng — mỗi node độc lập
    Database.set<object_t>(aClient, DB_PATH("telemetry"),
        object_t(_buildTelemetryJson(c).c_str()),              onUploadResult, "upTel");
    Database.set<object_t>(aClient, DB_PATH("analytics"),
        object_t(_buildAnalyticsJson(a).c_str()),              onUploadResult, "upAna");
    Database.set<object_t>(aClient, DB_PATH("relay_state"),
        object_t(_buildRelayJson(r).c_str()),                  onUploadResult, "upRly");
    Database.set<object_t>(aClient, DB_PATH("status"),
        object_t(_buildStatusJson(safeMode).c_str()), onUploadResult, "upSta");

    // water_change: set từng field riêng để không đụng manual_trigger của Web
    Database.set<String>  (aClient, DB_PATH("water_change/state"),
        String(_wcStateStr(waterChangeManager.getState())),
        onUploadResult, "upWC_st");
    Database.set<number_t>(aClient, DB_PATH("water_change/last_run"),
        number_t((double)waterChangeManager.lastRunDay(), 0),
        onUploadResult, "upWC_day");
    Database.set<number_t>(aClient, DB_PATH("water_change/last_run_ts"),
        number_t((double)waterChangeManager.lastRunTs(), 0),
        onUploadResult, "upWC_ts");
}

// ================================================================
// JSON BUILDERS
// ================================================================
String AquaFirebaseClient::_buildTelemetryJson(const CleanReading& c) {
    char b[512];
    snprintf(b, sizeof(b),
        "{\"timestamp\":%ld"
        ",\"temperature\":%.2f,\"ph\":%.3f,\"tds\":%.1f"
        ",\"temp_source\":\"%s\",\"ph_source\":\"%s\",\"tds_source\":\"%s\""
        ",\"temp_status\":\"%s\",\"ph_status\":\"%s\",\"tds_status\":\"%s\""
        ",\"shock_temp\":%s,\"shock_ph\":%s"
        ",\"fb_temp\":%d,\"fb_ph\":%d,\"fb_tds\":%d}",
        (long)time(nullptr),
        c.temperature, c.ph, c.tds,
        _dataSourceStr(c.source_temperature),
        _dataSourceStr(c.source_ph),
        _dataSourceStr(c.source_tds),
        _fieldStatusStr(c.status_temperature),
        _fieldStatusStr(c.status_ph),
        _fieldStatusStr(c.status_tds),
        c.shock_temperature ? "true" : "false",
        c.shock_ph          ? "true" : "false",
        (int)c.fallback_count_temp,
        (int)c.fallback_count_ph,
        (int)c.fallback_count_tds);
    return String(b);
}

String AquaFirebaseClient::_buildAnalyticsJson(const AnalyticsResult& a) {
    char b[256];
    snprintf(b, sizeof(b),
        "{\"ema_temp\":%.2f,\"ema_ph\":%.3f,\"ema_tds\":%.1f"
        ",\"drift_temp\":\"%s\",\"drift_ph\":\"%s\",\"drift_tds\":\"%s\""
        ",\"wsi\":%.1f,\"fsi\":%.2f}",
        a.ema_temp, a.ema_ph, a.ema_tds,
        _driftDirStr(a.drift_temp),
        _driftDirStr(a.drift_ph),
        _driftDirStr(a.drift_tds),
        a.wsi, a.fsi);
    return String(b);
}

String AquaFirebaseClient::_buildRelayJson(const RelayCommand& cmd) {
    char b[128];
    snprintf(b, sizeof(b),
        "{\"heater\":%s,\"cooler\":%s"
        ",\"ph_up\":%s,\"ph_down\":%s"
        ",\"pump_in\":%s,\"pump_out\":%s}",
        cmd.heater    ? "true" : "false",
        cmd.cooler    ? "true" : "false",
        cmd.ph_up     ? "true" : "false",
        cmd.ph_down   ? "true" : "false",
        cmd.pump_in   ? "true" : "false",
        cmd.pump_out  ? "true" : "false");
    return String(b);
}

String AquaFirebaseClient::_buildStatusJson(bool safeMode) {
    char b[160];
    snprintf(b, sizeof(b),
        "{\"uptime_s\":%lu,\"wifi_rssi\":%d,\"free_heap\":%lu"
        ",\"safe_mode\":%s}",
        (unsigned long)(millis() / 1000UL),
        wifiManager.rssi(),
        (unsigned long)ESP.getFreeHeap(),
        safeMode ? "true" : "false");
    return String(b);
}

// ================================================================
// UPLOAD HISTORY CHART — ghi 1 record vào RTDB mỗi 60s
// Path: /devices/{id}/history/chart/{unix_timestamp}
// Key là Unix timestamp (giây) → sort tự nhiên theo thời gian
// ================================================================
void AquaFirebaseClient::_uploadHistory(const CleanReading& c) {
    time_t now = time(nullptr);
    if (now < 1700000000L) return;  // NTP chưa sync → bỏ qua

    char path[72];
    snprintf(path, sizeof(path), DB_ROOT "/history/chart/%ld", (long)now);

    char body[80];
    snprintf(body, sizeof(body),
        "{\"temp\":%.2f,\"ph\":%.3f,\"tds\":%.1f}",
        c.temperature, c.ph, c.tds);

    Database.set<object_t>(aClient, path,
        object_t(body), onUploadResult, "upHist");

    LOG_DEBUG("FB", "History/chart: ts=%ld T=%.2f pH=%.3f TDS=%.1f",
              (long)now, c.temperature, c.ph, c.tds);
}

// ================================================================
// UPLOAD SHOCK EVENTS — chỉ gửi khi rising edge (false → true)
// Tránh gửi liên tục khi shock_flag kéo dài nhiều chu kỳ
//
// history/shock_event_ph/{ts}   → { ph_before, ph_after, is_read: false }
// history/shock_event_temp/{ts} → { temp_before, temp_after, is_read: false }
// ================================================================
void AquaFirebaseClient::_uploadShockEvents(const CleanReading& c) {
    if (!_ready || !wifiManager.isConnected()) return;

    time_t now = time(nullptr);
    if (now < 1700000000L) return;  // NTP chưa sync → bỏ qua

    // Rising edge shock_ph: false → true
    if (c.shock_ph && !_prevShockPh) {
        char path[80];
        snprintf(path, sizeof(path),
            DB_ROOT "/history/shock_event_ph/%ld", (long)now);

        char body[96];
        snprintf(body, sizeof(body),
            "{\"ph_before\":%.3f,\"ph_after\":%.3f,\"is_read\":false}",
            c.shock_ph_before, c.ph);

        Database.set<object_t>(aClient, path,
            object_t(body), onUploadResult, "upShkPh");

        LOG_WARNING("FB", "ShockEvent pH: %.3f → %.3f ts=%ld",
                    c.shock_ph_before, c.ph, (long)now);
    }

    // Rising edge shock_temperature: false → true
    if (c.shock_temperature && !_prevShockTemp) {
        char path[80];
        snprintf(path, sizeof(path),
            DB_ROOT "/history/shock_event_temp/%ld", (long)now);

        char body[96];
        snprintf(body, sizeof(body),
            "{\"temp_before\":%.2f,\"temp_after\":%.2f,\"is_read\":false}",
            c.shock_temp_before, c.temperature);

        Database.set<object_t>(aClient, path,
            object_t(body), onUploadResult, "upShkTmp");

        LOG_WARNING("FB", "ShockEvent TEMP: %.2f → %.2f ts=%ld",
                    c.shock_temp_before, c.temperature, (long)now);
    }

    // Cập nhật trạng thái trước cho chu kỳ tiếp theo
    _prevShockPh   = c.shock_ph;
    _prevShockTemp = c.shock_temperature;
}


// ================================================================
// PUSH SAFETY EVENT — ghi vào history/last_safety_event/{ts}
// Dùng timestamp làm key → log nhiều event, sort theo thời gian
// is_read: false mặc định — Web/App đánh dấu đã xem
// ================================================================
void AquaFirebaseClient::pushSafetyEvent(SafetyEvent evt) {
    if (!_ready || !wifiManager.isConnected()) return;

    time_t now = time(nullptr);
    if (now < 1700000000L) return;  // NTP chưa sync → bỏ qua

    char path[80];
    snprintf(path, sizeof(path),
        DB_ROOT "/history/last_safety_event/%ld", (long)now);

    char body[80];
    snprintf(body, sizeof(body),
        "{\"event\":\"%s\",\"is_read\":false}",
        _safetyEventStr(evt));

    Database.set<object_t>(aClient, path,
        object_t(body), onUploadResult, "pushEvt");

    LOG_INFO("FB", "SafetyEvent → history: %s ts=%ld",
             _safetyEventStr(evt), (long)now);
}

// ================================================================
// TRIGGER SOURCE
// ================================================================
void AquaFirebaseClient::notifyButtonTrigger()   { _writeTriggerSource("BUTTON");   }
void AquaFirebaseClient::notifyScheduleTrigger() { _writeTriggerSource("SCHEDULE"); }
void AquaFirebaseClient::notifyTriggerDone()     { _writeTriggerSource("NONE");     }
void AquaFirebaseClient::_notifyWebTrigger()     { _writeTriggerSource("WEB");      }

void AquaFirebaseClient::_writeTriggerSource(const char* src) {
    if (!_ready || !wifiManager.isConnected()) return;
    Database.set<String>(aClient,
        DB_PATH("water_change/trigger_source"),
        String(src), onUploadResult, "trigSrc");
    LOG_DEBUG("FB", "trigger_source=\"%s\"", src);
}

// ================================================================
// STREAM 1 — /settings (SSE)
// ================================================================
void AquaFirebaseClient::_startSettingsStream() {
    Database.get(aClientStream1,
        DB_PATH("settings"),
        onStream1Result,
        true,        // SSE mode
        "stream1");
    LOG_INFO("FB", "Stream1: %s", DB_PATH("settings"));
}

// ================================================================
// STREAM 2 — /water_change/manual_trigger (SSE)
// ================================================================
void AquaFirebaseClient::_startTriggerStream() {
    Database.get(aClientStream2,
        DB_PATH("water_change/manual_trigger"),
        onStream2Result,
        true,        // SSE mode
        "stream2");
    LOG_INFO("FB", "Stream2: %s",
             DB_PATH("water_change/manual_trigger"));
}

// ================================================================
// STREAM HEALTH CHECK
// ================================================================
void AquaFirebaseClient::_checkStreamHealth() {
    uint32_t now = millis();
    if (now - _lastSettingsStreamMs > FIREBASE_STREAM_RETRY_MS) {
        LOG_WARNING("FB", "Stream1 stale → restart");
        _startSettingsStream();
        _lastSettingsStreamMs = now;
    }
    if (now - _lastTriggerStreamMs > FIREBASE_STREAM_RETRY_MS) {
        LOG_WARNING("FB", "Stream2 stale → restart");
        _startTriggerStream();
        _lastTriggerStreamMs = now;
    }
}

// ================================================================
// PARSE /settings PAYLOAD
//
// 3 cases:
//   path == "/"                → full dump
//   path == "/config"          → group-level object
//   path == "/config/temp_min" → single field
// ================================================================
void AquaFirebaseClient::_onSettingsPayload(const char* path, const char* data) {
    if (!path || !data || !*data) return;

    auto applyGroup = [](const char* groupName, const char* groupJson) {
        if (strcmp(groupName, "config") == 0) {
            ControlConfig cc = configManager.getControlConfig();
            if (configManager.parseControlConfigJson(groupJson, cc)) {
                configManager.applyControlConfig(cc);
                LOG_INFO("FB", "Stream: config applied");
            } else {
                LOG_WARNING("FB", "Stream: config parse failed json=%s", groupJson);
            }
        } else if (strcmp(groupName, "pipeline_config") == 0) {
            PipelineConfig pc = configManager.getPipelineConfig();
            if (configManager.parsePipelineConfigJson(groupJson, pc)) {
                configManager.applyPipelineConfig(pc);
                LOG_INFO("FB", "Stream: pipeline_config applied");
            } else {
                LOG_WARNING("FB", "Stream: pipeline_config parse failed");
            }
        } else if (strcmp(groupName, "analytics_config") == 0) {
            AnalyticsConfig ac = analytics.getConfig();
            float v;
            auto pF = [&](const char* k, float& out, float lo, float hi) {
                const char* p = strstr(groupJson, k);
                if (!p) return;
                p += strlen(k);
                while (*p=='"'||*p==':'||*p==' ') p++;
                if (sscanf(p, "%f", &v)==1 && v>=lo && v<=hi) out = v;
            };
            pF("\"ema_alpha\"",       ac.ema_alpha,       0.001f,  0.5f);
            pF("\"cusum_k\"",         ac.cusum_k,         0.001f, 10.0f);
            pF("\"cusum_threshold\"", ac.cusum_threshold, 0.001f,100.0f);
            analytics.setConfig(ac);
            LOG_INFO("FB", "Stream: analytics_config applied");
        } else if (strcmp(groupName, "safety_limits") == 0) {
            SafetyLimits sl = safetyCore.getLimits();
            float fv; int iv;
            auto pF = [&](const char* k, float& out) {
                const char* p = strstr(groupJson, k);
                if (!p) return;
                p += strlen(k);
                while (*p=='"'||*p==':'||*p==' ') p++;
                if (sscanf(p, "%f", &fv)==1) out = fv;
            };
            auto pU = [&](const char* k, uint32_t& out) {
                const char* p = strstr(groupJson, k);
                if (!p) return;
                p += strlen(k);
                while (*p=='"'||*p==':'||*p==' ') p++;
                if (sscanf(p, "%d", &iv)==1 && iv>0) out = (uint32_t)iv;
            };
            pF("\"thermal_cutoff_c\"",        sl.thermal_cutoff_c);
            pF("\"temp_emergency_cool_c\"",   sl.temp_emergency_cool_c);
            pU("\"heater_max_runtime_ms\"",   sl.heater_max_runtime_ms);
            pU("\"heater_cooldown_ms\"",      sl.heater_cooldown_ms);
            pU("\"ph_pump_max_pulse_ms\"",    sl.ph_pump_max_pulse_ms);
            pU("\"ph_pump_min_interval_ms\"", sl.ph_pump_min_interval_ms);
            const char* p2 = strstr(groupJson, "\"stale_sensor_threshold\"");
            if (p2) {
                p2 += strlen("\"stale_sensor_threshold\"");
                while (*p2=='"'||*p2==':'||*p2==' ') p2++;
                if (sscanf(p2, "%d", &iv)==1 && iv>0)
                    sl.stale_sensor_threshold = (uint8_t)iv;
            }
            if (safetyCore.setLimits(sl))
                LOG_INFO("FB", "Stream: safety_limits applied");
        } else if (strcmp(groupName, "water_schedule") == 0) {
            WaterChangeSchedule ws = configManager.getWaterSchedule();
            if (configManager.parseWaterScheduleJson(groupJson, ws)) {
                configManager.applyWaterSchedule(ws);
                LOG_INFO("FB", "Stream: water_schedule applied");
            } else {
                LOG_WARNING("FB", "Stream: water_schedule parse failed");
            }
        } else if (strcmp(groupName, "calibration") == 0) {
            SensorCalibration calib = configManager.getCalibration();
            if (configManager.parseCalibrationJson(groupJson, calib)) {
                configManager.applyCalibration(calib);
                LOG_INFO("FB", "Stream: calibration applied");
            } else {
                LOG_WARNING("FB", "Stream: calibration parse failed json=%s", groupJson);
            }
        }
    };

    // Case 1: path == "/" → full /settings dump
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        LOG_INFO("FB", "Stream1: full settings dump");
        auto findSub = [](const char* s, const char* key) -> const char* {
            const char* p = strstr(s, key);
            if (!p) return nullptr;
            return strchr(p + strlen(key), '{');
        };
        const char* groups[] = {
            "config", "pipeline_config", "analytics_config",
            "safety_limits", "water_schedule", "calibration"
        };
        for (const char* g : groups) {
            char sk[32];
            snprintf(sk, sizeof(sk), "\"%s\"", g);
            const char* sub = findSub(data, sk);
            if (sub) applyGroup(g, sub);
        }
        return;
    }

    // Bỏ '/' đầu
    const char* groupName = (path[0] == '/') ? path + 1 : path;

    // Case 2: path == "/config" → group-level object
    const char* slash = strchr(groupName, '/');
    if (!slash) {
        applyGroup(groupName, data);
        return;
    }

    // Case 3: path == "/config/temp_min" → single field
    char group[32] = {};
    size_t gLen = (size_t)(slash - groupName);
    if (gLen >= sizeof(group)) return;
    memcpy(group, groupName, gLen);
    const char* fieldName = slash + 1;

    char wrapped[128];
    snprintf(wrapped, sizeof(wrapped), "{\"%s\":%s}", fieldName, data);
    LOG_DEBUG("FB", "Stream1 field: group=%s field=%s val=%s", group, fieldName, data);
    applyGroup(group, wrapped);
}

// ================================================================
// PROCESS MANUAL TRIGGER
// ================================================================
void AquaFirebaseClient::_onTriggerPayload(bool triggered) {
    if (!triggered) return;

    LOG_INFO("FB", "manual_trigger=true (Web)");
    _notifyWebTrigger();

    if (!waterChangeManager.isBusy()) {
        waterChangeManager.triggerManual();
        LOG_INFO("FB", "Water change triggered via Web");
    } else {
        LOG_WARNING("FB", "manual_trigger ignored: busy (%s)",
                    _wcStateStr(waterChangeManager.getState()));
    }

    // Reset manual_trigger → false
    Database.set<bool>(aClient,
        DB_PATH("water_change/manual_trigger"),
        false, onUploadResult, "resetTrig");
}

// ================================================================
// ================================================================
// FREE FUNCTION CALLBACKS
// ================================================================
// ================================================================

static void onUploadResult(AsyncResult& r) {
    if (r.isError())
        LOG_WARNING("FB", "[%s] %s (code=%d)",
                    r.uid().c_str(),
                    r.error().message().c_str(),
                    r.error().code());
}

static void onStream1Result(AsyncResult& r) {
    if (!_fbClient) return;

    if (r.isError()) {
        LOG_WARNING("FB", "Stream1 err: %s (code=%d)",
                    r.error().message().c_str(), r.error().code());
        return;
    }
    if (!r.available()) return;

    _fbClient->_lastSettingsStreamMs = millis();

    RealtimeDatabaseResult& RTDB = r.to<RealtimeDatabaseResult>();
    if (RTDB.event() != "put" && RTDB.event() != "patch") return;

    const String& dataPath = RTDB.dataPath();
    const char*   dataStr  = r.c_str();
    if (!dataStr) return;

    LOG_DEBUG("FB", "Stream1 path=%s", dataPath.c_str());
    _fbClient->_onSettingsPayload(dataPath.c_str(), dataStr);
}

static void onStream2Result(AsyncResult& r) {
    if (!_fbClient) return;

    if (r.isError()) {
        LOG_WARNING("FB", "Stream2 err: %s (code=%d)",
                    r.error().message().c_str(), r.error().code());
        return;
    }
    if (!r.available()) return;

    _fbClient->_lastTriggerStreamMs = millis();

    RealtimeDatabaseResult& RTDB = r.to<RealtimeDatabaseResult>();
    if (RTDB.event() == "put" || RTDB.event() == "patch") {
        _fbClient->_onTriggerPayload(RTDB.to<bool>());
    }
}

// ================================================================
// ENUM → STRING
// ================================================================
const char* AquaFirebaseClient::_wcStateStr(WaterChangeState s) {
    switch (s) {
        case WaterChangeState::IDLE:        return "IDLE";
        case WaterChangeState::PUMPING_OUT: return "PUMPING_OUT";
        case WaterChangeState::PUMPING_IN:  return "PUMPING_IN";
        case WaterChangeState::DONE:        return "DONE";
        default:                            return "IDLE";
    }
}

const char* AquaFirebaseClient::_safetyEventStr(SafetyEvent e) {
    switch (e) {
        case SafetyEvent::NONE:                 return "NONE";
        case SafetyEvent::THERMAL_CUTOFF:       return "THERMAL_CUTOFF";
        case SafetyEvent::EMERGENCY_COOL:       return "EMERGENCY_COOL";
        case SafetyEvent::HEATER_RUNTIME_LIMIT: return "HEATER_RUNTIME_LIMIT";
        case SafetyEvent::HEATER_COOLDOWN:      return "HEATER_COOLDOWN";
        case SafetyEvent::SENSOR_UNRELIABLE:    return "SENSOR_UNRELIABLE";
        case SafetyEvent::SENSOR_STALE:         return "SENSOR_STALE";
        case SafetyEvent::MUTUAL_EXCLUSION:     return "MUTUAL_EXCLUSION";
        case SafetyEvent::PH_PUMP_INTERVAL:     return "PH_PUMP_INTERVAL";
        case SafetyEvent::SHOCK_GUARD:          return "SHOCK_GUARD";
        default:                                return "NONE";
    }
}

const char* AquaFirebaseClient::_dataSourceStr(DataSource s) {
    switch (s) {
        case DataSource::MEASURED:         return "MEASURED";
        case DataSource::FALLBACK_LAST:    return "FALLBACK_LAST";
        case DataSource::FALLBACK_MEDIAN:  return "FALLBACK_MEDIAN";
        case DataSource::FALLBACK_DEFAULT: return "FALLBACK_DEFAULT";
        default:                           return "MEASURED";
    }
}

const char* AquaFirebaseClient::_fieldStatusStr(FieldStatus s) {
    switch (s) {
        case FieldStatus::OK:           return "OK";
        case FieldStatus::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case FieldStatus::MAD_OUTLIER:  return "MAD_OUTLIER";
        case FieldStatus::SENSOR_ERROR: return "SENSOR_ERROR";
        default:                        return "OK";
    }
}

const char* AquaFirebaseClient::_driftDirStr(DriftDir d) {
    switch (d) {
        case DriftDir::NONE: return "NONE";
        case DriftDir::UP:   return "UP";
        case DriftDir::DOWN: return "DOWN";
        default:             return "NONE";
    }
}