// ================================================================
// wifi_firebase.cpp — Intelligent Aquarium v4.2 (PhDose update)
//
// Thay đổi so với v4.1:
//   - Bỏ PID gains khỏi settings/config
//   + Thêm group "ph_dose_config" trong stream settings
//   + _uploadAll: thêm ph_session node (state, last_median_ph, last_pulse_ms)
//   + applyControlConfig: không còn gọi pidCtrl.setConfig()
// ================================================================

#include "wifi_firebase.h"
#include "credentials.h"
#include "logger.h"
#include "config_manager.h"
#include "data_pipeline.h"
#include "analytics.h"
#include "safety_core.h"
#include "water_change_manager.h"
#include "ph_dose_controller.h"
#include "ph_session_manager.h"
#include "system_constants.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

extern DataPipeline dataPipeline;

using AsyncClient = AsyncClientClass;

// ----------------------------------------------------------------
// SSL contexts (riêng biệt cho upload vs 2 stream)
// ----------------------------------------------------------------
static WiFiClientSecure ssl_upload;
static WiFiClientSecure ssl_stream1;
static WiFiClientSecure ssl_stream2;

// ----------------------------------------------------------------
// Firebase objects
// ----------------------------------------------------------------
static LegacyToken legacy_auth(FIREBASE_TOKEN);
static FirebaseApp app;
static RealtimeDatabase Database;

static AsyncClient aClient      (ssl_upload);
static AsyncClient aClientStream1(ssl_stream1);
static AsyncClient aClientStream2(ssl_stream2);

// ----------------------------------------------------------------
// Global pointer cho callbacks
// ----------------------------------------------------------------
static AquaFirebaseClient* _fbClient = nullptr;

// ----------------------------------------------------------------
// Free-function callbacks
// ----------------------------------------------------------------
static void onUploadResult(AsyncResult& r) {
    if (r.isError()) {
        LOG_WARNING("FB", "Upload err: %s (code=%d)",
                    r.error().message().c_str(),
                    r.error().code());
    }
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
    _fbClient->_onSettingsPayload(RTDB.dataPath().c_str(), r.c_str());
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
// WiFiManager
// ================================================================
WiFiManager wifiManager;

WiFiManager::WiFiManager()
    : _ssid(nullptr), _password(nullptr),
      _lastReconnectMs(0), _wasConnected(false) {}

void WiFiManager::begin(const char* ssid, const char* password) {
    _ssid     = ssid;
    _password = password;
    _connect();
}

void WiFiManager::_connect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _password);
    LOG_INFO("WIFI", "Connecting to %s...", _ssid);
}

void WiFiManager::loop() {
    bool connected = isConnected();
    if (!connected) {
        uint32_t now = millis();
        if (now - _lastReconnectMs > WIFI_RECONNECT_INTERVAL_MS) {
            _lastReconnectMs = now;
            _connect();
        }
    }
    if (connected && !_wasConnected)
        LOG_INFO("WIFI", "Connected, IP=%s", WiFi.localIP().toString().c_str());
    if (!connected && _wasConnected)
        LOG_WARNING("WIFI", "Disconnected");
    _wasConnected = connected;
}

bool WiFiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }
int  WiFiManager::rssi()        const { return WiFi.RSSI(); }

// ================================================================
// AquaFirebaseClient
// ================================================================
AquaFirebaseClient firebaseClient;

AquaFirebaseClient::AquaFirebaseClient()
    : _ready(false), _lastUploadMs(0), _lastHistoryMs(0),
      _lastSettingsStreamMs(0), _lastTriggerStreamMs(0),
      _prevShockTemp(false),
      _phSensorError(false),
      _prevHistTemp(NAN), _prevHistTds(NAN), _prevHistPh(NAN) {}

// ================================================================
// BEGIN
// ================================================================
void AquaFirebaseClient::begin() {
    if (_ready) return;
    _fbClient = this;

    ssl_upload.setInsecure();
    ssl_stream1.setInsecure();
    ssl_stream2.setInsecure();

    initializeApp(aClient, app, getAuth(legacy_auth), onUploadResult, "authTask");
    app.getApp<RealtimeDatabase>(Database);
    Database.url(FIREBASE_URL);

    _startSettingsStream();
    _startTriggerStream();

    _ready = true;
    uint32_t now          = millis();
    _lastUploadMs         = 0;
    _lastHistoryMs        = 0;
    _lastSettingsStreamMs = now;
    _lastTriggerStreamMs  = now;

    LOG_INFO("FB", "Ready. Device=%s", FIREBASE_DEVICE);
}

void AquaFirebaseClient::restart() {
    _ready = false;
    begin();
    LOG_INFO("FB", "Restarted (heap=%lu)", (unsigned long)ESP.getFreeHeap());
}

// ================================================================
// LOOP
// ================================================================
void AquaFirebaseClient::loop(
    const CleanReading&    clean,
    const AnalyticsResult& analyticsResult,
    const RelayCommand&    relayState,
    SafetyEvent            lastSafetyEvent,
    bool                   safeMode)
{
    if (!_ready || !wifiManager.isConnected()) return;

    app.loop();
    Database.loop();

    uint32_t now = millis();

    if (now - _lastUploadMs >= FIREBASE_UPLOAD_INTERVAL_MS) {
        _lastUploadMs = now;
        LOG_DEBUG("FB", "heap=%lu minHeap=%lu",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMinFreeHeap());
        _uploadAll(clean, analyticsResult, relayState, lastSafetyEvent, safeMode);
    }

    if (now - _lastHistoryMs >= FIREBASE_HISTORY_INTERVAL_MS) {
        _lastHistoryMs = now;
        _uploadHistory(clean);
    }

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
    Database.set<object_t>(aClient, DB_PATH("telemetry"),
        object_t(_buildTelemetryJson(c).c_str()),   onUploadResult, "upTel");
    Database.set<object_t>(aClient, DB_PATH("analytics"),
        object_t(_buildAnalyticsJson(a).c_str()),   onUploadResult, "upAna");
    Database.set<object_t>(aClient, DB_PATH("relay_state"),
        object_t(_buildRelayJson(r).c_str()),        onUploadResult, "upRly");
    Database.set<object_t>(aClient, DB_PATH("status"),
        object_t(_buildStatusJson(safeMode).c_str()), onUploadResult, "upSta");

    // Water change
    Database.set<String>  (aClient, DB_PATH("water_change/state"),
        String(_wcStateStr(waterChangeManager.getState())),
        onUploadResult, "upWC_st");
    Database.set<number_t>(aClient, DB_PATH("water_change/last_run"),
        number_t((double)waterChangeManager.lastRunDay(), 0),
        onUploadResult, "upWC_day");
    Database.set<number_t>(aClient, DB_PATH("water_change/last_run_ts"),
        number_t((double)waterChangeManager.lastRunTs(), 0),
        onUploadResult, "upWC_ts");

    // pH Session state — mới
    Database.set<object_t>(aClient, DB_PATH("ph_session"),
        object_t(_buildPhSessionJson().c_str()), onUploadResult, "upPhSess");
}

// ================================================================
// JSON BUILDERS
String AquaFirebaseClient::_buildTelemetryJson(const CleanReading& c) {
    // pH không còn trong CleanReading — hiển thị từ /ph_session riêng.
    char b[380];
    snprintf(b, sizeof(b),
        "{\"timestamp\":%ld"
        ",\"temperature\":%.1f,\"tds\":%.1f"
        ",\"temp_source\":\"%s\",\"tds_source\":\"%s\""
        ",\"temp_status\":\"%s\",\"tds_status\":\"%s\""
        ",\"shock_temp\":%s"
        ",\"fb_temp\":%d,\"fb_tds\":%d}",
        (long)time(nullptr),
        c.temperature, c.tds,
        _dataSourceStr(c.source_temperature),
        _dataSourceStr(c.source_tds),
        _fieldStatusStr(c.status_temperature),
        _fieldStatusStr(c.status_tds),
        c.shock_temperature ? "true" : "false",
        (int)c.fallback_count_temp,
        (int)c.fallback_count_tds);
    return String(b);
}


String AquaFirebaseClient::_buildAnalyticsJson(const AnalyticsResult& a) {
    char b[200];
    snprintf(b, sizeof(b),
        "{\"ema_temp\":%.1f,\"ema_tds\":%.1f"
        ",\"drift_temp\":\"%s\",\"drift_tds\":\"%s\""
        ",\"wsi\":%.1f,\"fsi\":%.2f}",
        a.ema_temp, a.ema_tds,
        _driftDirStr(a.drift_temp),
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
// pH session — nguồn dữ liệu pH duy nhất cho dashboard
String AquaFirebaseClient::_buildPhSessionJson() {
    const PhDoseResult& dose = phSessionMgr.lastDoseResult();
    const PhDoseConfig& cfg  = phDoseCtrl.getDoseConfig();
    float medPh = phSessionMgr.lastMedianPh();

    char phBuf[12];
    if (isnan(medPh)) snprintf(phBuf, sizeof(phBuf), "null");
    else              snprintf(phBuf, sizeof(phBuf), "%.1f", medPh);

    // sensor_error nằm trong object ph_session — ghi cùng object, không bao giờ bị xóa
    const char* sensorErr = _phSensorError ? "\"true\"" : "\"false\"";

    char b[380];
    snprintf(b, sizeof(b),
        "{\"state\":\"%s\""
        ",\"next_session_s\":%lu"
        ",\"last_median_ph\":%s"
        ",\"last_session_ts\":%ld"
        ",\"last_pulse_ms\":%lu"
        ",\"last_direction\":\"%s\""
        ",\"last_overshoot\":%.3f"
        ",\"measure_interval_s\":%lu"
        ",\"session_duration_s\":%lu"
        ",\"warmup_s\":%lu"
        ",\"sensor_error\":%s}",
        phSessionMgr.stateStr(),
        (unsigned long)phSessionMgr.secondsUntilNextSession(),
        phBuf,
        (long)phSessionMgr.lastSessionTs(),
        (unsigned long)dose.pulse_ms,
        dose.ph_up ? "UP" : dose.ph_down ? "DOWN" : "NONE",
        dose.overshoot,
        (unsigned long)(cfg.measure_interval_ms / 1000),
        (unsigned long)(cfg.session_duration_ms / 1000),
        (unsigned long)(cfg.warmup_ms / 1000),
        sensorErr);
    return String(b);
}


// ================================================================
// UPLOAD HISTORY CHART — ghi mỗi 60s
// Mỗi field chỉ được include vào JSON khi giá trị thay đổi so với
// lần trước → chart sạch, không có flat-line giả.
// pH lấy từ phSessionMgr.lastMedianPh() (đo 5 phút/lần).
// Timestamp luôn được ghi để chart có điểm liên tục.
// ================================================================
void AquaFirebaseClient::_uploadHistory(const CleanReading& c) {
    time_t now = time(nullptr);
    if (now < 1700000000L) return;

    float ph = phSessionMgr.lastMedianPh();

    // Xác định field nào thay đổi (NAN = chưa có lần trước → luôn ghi)
    bool tempChanged = isnan(_prevHistTemp) || fabsf(c.temperature - _prevHistTemp) > 0.01f;
    bool tdsChanged  = isnan(_prevHistTds)  || fabsf(c.tds         - _prevHistTds)  > 0.1f;
    bool phChanged   = !isnan(ph) &&
                       (isnan(_prevHistPh)  || fabsf(ph            - _prevHistPh)   > 0.001f);

    // Không có field nào thay đổi → skip, không ghi node rỗng
    if (!tempChanged && !tdsChanged && !phChanged) return;

    // Build JSON chỉ với field thay đổi (timestamp luôn có)
    char body[128];
    char* p = body;
    p += snprintf(p, sizeof(body), "{\"ts\":%ld", (long)now);
    if (tempChanged) p += snprintf(p, body + sizeof(body) - p, ",\"temp\":%.1f",  c.temperature);
    if (tdsChanged)  p += snprintf(p, body + sizeof(body) - p, ",\"tds\":%.1f",   c.tds);
    if (phChanged)   p += snprintf(p, body + sizeof(body) - p, ",\"ph\":%.1f",    ph);
    snprintf(p, body + sizeof(body) - p, "}");

    char path[72];
    snprintf(path, sizeof(path), DB_ROOT "/history/chart/%ld", (long)now);
    Database.set<object_t>(aClient, path, object_t(body), onUploadResult, "upHist");

    // Cập nhật prev chỉ khi field thực sự được ghi
    if (tempChanged) _prevHistTemp = c.temperature;
    if (tdsChanged)  _prevHistTds  = c.tds;
    if (phChanged)   _prevHistPh   = ph;

    LOG_DEBUG("FB", "History: %s%s%s",
              tempChanged ? "T " : "", tdsChanged ? "TDS " : "", phChanged ? "pH" : "");
}

// ================================================================
// UPLOAD SHOCK EVENTS — rising edge only
// pH shock: log trực tiếp từ PhSessionManager (logPhShockEvent)
// Temp: { temp_before, temp_after, is_read }
// ================================================================
void AquaFirebaseClient::_uploadShockEvents(const CleanReading& c) {
    if (!_ready || !wifiManager.isConnected()) return;
    time_t now = time(nullptr);
    if (now < 1700000000L) return;

    // Rising edge shock_temperature: false → true
    if (c.shock_temperature && !_prevShockTemp) {
        char path[80], body[100];
        snprintf(path, sizeof(path),
            DB_ROOT "/history/shock_event_temp/%ld", (long)now);
        snprintf(body, sizeof(body),
            "{\"temp_before\":%.1f,\"temp_after\":%.1f,\"is_read\":false}",
            c.shock_temp_before, c.temperature);
        Database.set<object_t>(aClient, path, object_t(body), onUploadResult, "upShTmp");
        LOG_WARNING("FB", "ShockEvent TEMP: %.2f → %.2f", c.shock_temp_before, c.temperature);
    }
    _prevShockTemp = c.shock_temperature;
}

// ================================================================
// PH SESSION EVENT LOGGERS — gọi từ PhSessionManager
// ================================================================
void AquaFirebaseClient::clearPhSensorErrorFlag() {
    if (!_phSensorError) return;
    _phSensorError = false;
    // Ghi ngay để dashboard biết lỗi đã qua
    String json = _buildPhSessionJson();
    Database.set<object_t>(aClient, DB_PATH("ph_session"),
                           object_t(json.c_str()), onUploadResult, "upPhSessOk");
    LOG_INFO("FB", "pH sensor_error cleared");
}

void AquaFirebaseClient::logPhSensorError(float spread, float threshold, uint8_t samples) {
    _phSensorError = true;
    // Ghi ngay lập tức — không chờ _uploadAll vì _uploadAll có thể reset về false trước
    String json = _buildPhSessionJson();
    Database.set<object_t>(aClient, DB_PATH("ph_session"),
                           object_t(json.c_str()), onUploadResult, "upPhSessErr");
    LOG_WARNING("FB", "pH sensor_error: spread=%.3f threshold=%.3f samples=%d",
                spread, threshold, (int)samples);
}



void AquaFirebaseClient::logPhShockEvent(float phBefore, float phAfter, float delta) {
    if (!_ready || !wifiManager.isConnected()) return;
    time_t now = time(nullptr);
    if (now < 1700000000L) return;

    char path[96], body[128];
    snprintf(path, sizeof(path),
        DB_ROOT "/history/shock_event_ph/%ld", (long)now);
    snprintf(body, sizeof(body),
        "{\"ph_before\":%.1f,\"ph_after\":%.1f,\"delta\":%.1f,\"is_read\":false}",
        phBefore, phAfter, delta);
    Database.set<object_t>(aClient, path, object_t(body), onUploadResult, "phShock");
    LOG_WARNING("FB", "ShockEvent pH: %.3f → %.3f (delta=%.3f)", phBefore, phAfter, delta);
}

// ================================================================
// PUSH SAFETY EVENT
// ================================================================
void AquaFirebaseClient::pushSafetyEvent(SafetyEvent evt) {
    if (!_ready || !wifiManager.isConnected()) return;
    time_t now = time(nullptr);
    if (now < 1700000000L) return;

    char path[80], body[80];
    snprintf(path, sizeof(path), DB_ROOT "/history/last_safety_event/%ld", (long)now);
    snprintf(body, sizeof(body), "{\"event\":\"%s\",\"is_read\":false}", _safetyEventStr(evt));
    Database.set<object_t>(aClient, path, object_t(body), onUploadResult, "pushEvt");
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
}

// ================================================================
// STREAMS
// ================================================================
void AquaFirebaseClient::_startSettingsStream() {
    Database.get(aClientStream1, DB_PATH("settings"),
        onStream1Result, true, "stream1");
    LOG_INFO("FB", "Stream1: %s", DB_PATH("settings"));
}

void AquaFirebaseClient::_startTriggerStream() {
    Database.get(aClientStream2, DB_PATH("water_change/manual_trigger"),
        onStream2Result, true, "stream2");
    LOG_INFO("FB", "Stream2: %s", DB_PATH("water_change/manual_trigger"));
}

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
// ================================================================
void AquaFirebaseClient::_onSettingsPayload(const char* path, const char* data) {
    if (!path || !data || !*data) return;

    // ── Helper: parse group JSON và apply ────────────────────────
    auto applyGroup = [](const char* groupName, const char* groupJson) {

        // ── config (nhiệt độ, pH range, TDS — KHÔNG còn PID) ────
        if (strcmp(groupName, "config") == 0) {
            ControlConfig cc = configManager.getControlConfig();
            if (configManager.parseControlConfigJson(groupJson, cc)) {
                configManager.applyControlConfig(cc);
                LOG_INFO("FB", "Stream: config applied");
            } else {
                LOG_WARNING("FB", "Stream: config parse failed");
            }

        // ── ph_dose_config ────────────────────────────────────────
        } else if (strcmp(groupName, "ph_dose_config") == 0) {
            PhDoseConfig dc = phDoseCtrl.getDoseConfig();
            if (configManager.parsePhDoseConfigJson(groupJson, dc)) {
                configManager.applyPhDoseConfig(dc);
                LOG_INFO("FB", "Stream: ph_dose_config applied");
            } else {
                LOG_WARNING("FB", "Stream: ph_dose_config parse failed");
            }

        // ── pipeline_config ───────────────────────────────────────
        } else if (strcmp(groupName, "pipeline_config") == 0) {
            PipelineConfig pc = configManager.getPipelineConfig();
            if (configManager.parsePipelineConfigJson(groupJson, pc)) {
                configManager.applyPipelineConfig(pc);
                LOG_INFO("FB", "Stream: pipeline_config applied");
            } else {
                LOG_WARNING("FB", "Stream: pipeline_config parse failed");
            }

        // ── analytics_config ──────────────────────────────────────
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

        // ── safety_limits ────────────────────────────────────────
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

        // ── water_schedule ────────────────────────────────────────
        } else if (strcmp(groupName, "water_schedule") == 0) {
            WaterChangeSchedule ws = configManager.getWaterSchedule();
            if (configManager.parseWaterScheduleJson(groupJson, ws)) {
                configManager.applyWaterSchedule(ws);
                LOG_INFO("FB", "Stream: water_schedule applied");
            } else {
                LOG_WARNING("FB", "Stream: water_schedule parse failed");
            }

        // ── calibration ───────────────────────────────────────────
        } else if (strcmp(groupName, "calibration") == 0) {
            SensorCalibration calib = configManager.getCalibration();
            if (configManager.parseCalibrationJson(groupJson, calib)) {
                configManager.applyCalibration(calib);
                LOG_INFO("FB", "Stream: calibration applied");
            } else {
                LOG_WARNING("FB", "Stream: calibration parse failed");
            }
        }
    };

    // ── Case 1: full dump (path == "/") ──────────────────────────
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        LOG_INFO("FB", "Stream1: full settings dump");
        auto findSub = [](const char* s, const char* key) -> const char* {
            const char* p = strstr(s, key);
            if (!p) return nullptr;
            return strchr(p + strlen(key), '{');
        };
        // Thêm ph_dose_config vào danh sách groups
        const char* groups[] = {
            "config", "ph_dose_config", "pipeline_config", "analytics_config",
            "safety_limits", "water_schedule", "calibration"
        };
        for (const char* g : groups) {
            char sk[40];
            snprintf(sk, sizeof(sk), "\"%s\"", g);
            const char* sub = findSub(data, sk);
            if (sub) applyGroup(g, sub);
        }
        return;
    }

    // ── Case 2/3: single group hoặc single field ──────────────────
    const char* groupName = (path[0] == '/') ? path + 1 : path;
    const char* slash = strchr(groupName, '/');

    if (!slash) {
        // Đúng group-level (e.g. "/config", "/ph_dose_config")
        applyGroup(groupName, data);
    } else {
        // Single-field update — extract group rồi re-apply group
        char group[40] = {};
        size_t len = (size_t)(slash - groupName);
        if (len < sizeof(group)) {
            memcpy(group, groupName, len);
            group[len] = '\0';
            // Không thể apply single-field từ stream JSON partial
            // → log và bỏ qua (full dump sẽ sync sau)
            LOG_DEBUG("FB", "Stream: single-field update ignored for %s/%s — wait for full dump",
                      group, slash + 1);
        }
    }
}

// ================================================================
// TRIGGER PAYLOAD (stream 2)
// ================================================================
void AquaFirebaseClient::_onTriggerPayload(bool triggered) {
    if (!triggered) return;
    LOG_INFO("FB", "Web trigger received → water change");
    waterChangeManager.triggerManual();
    _notifyWebTrigger();
    Database.set<bool>(aClient,
        DB_PATH("water_change/manual_trigger"),
        false, onUploadResult, "resetTrig");
}

// ================================================================
// ENUM → STRING helpers
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
        case DataSource::MEASURED:          return "MEASURED";
        case DataSource::FALLBACK_LAST:     return "FALLBACK_LAST";
        case DataSource::FALLBACK_MEDIAN:   return "FALLBACK_MEDIAN";
        case DataSource::FALLBACK_DEFAULT:  return "FALLBACK_DEFAULT";
        default:                            return "UNKNOWN";
    }
}

const char* AquaFirebaseClient::_fieldStatusStr(FieldStatus s) {
    switch (s) {
        case FieldStatus::OK:           return "OK";
        case FieldStatus::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case FieldStatus::MAD_OUTLIER:  return "MAD_OUTLIER";
        case FieldStatus::SENSOR_ERROR: return "SENSOR_ERROR";
        default:                        return "UNKNOWN";
    }
}

const char* AquaFirebaseClient::_driftDirStr(DriftDir d) {
    switch (d) {
        case DriftDir::UP:   return "UP";
        case DriftDir::DOWN: return "DOWN";
        default:             return "NONE";
    }
}