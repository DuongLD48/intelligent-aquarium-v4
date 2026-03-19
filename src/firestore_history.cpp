// ================================================================
// firestore_history.cpp — Intelligent Aquarium v4.1
// ================================================================

// wifi_firebase.h phải include TRƯỚC FirebaseClient.h
// để ENABLE_DATABASE + ENABLE_FIRESTORE có hiệu lực
#include "wifi_firebase.h"
#include "firestore_history.h"
#include "credentials.h"
#include "logger.h"

// Include SAU wifi_firebase.h
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <math.h>

// Dùng namespace của thư viện để tránh ambiguous
using namespace firebase_ns;

// ── Firestore objects ────────────────────────────────────────────
// KHÔNG tạo WiFiClientSecure mới — tái dụng aClient từ wifi_firebase.cpp
// để tránh SSL memory allocation failed (ESP32 chỉ chịu ~3 SSL context)
static Firestore::Documents Docs;

// ── Singleton ────────────────────────────────────────────────────
FirestoreHistory firestoreHistory;

// ── Callback ─────────────────────────────────────────────────────
static void onFstoreResult(AsyncResult& r) {
    if (r.isError())
        LOG_WARNING("FSH", "[%s] err=%s code=%d",
            r.uid().c_str(),
            r.error().message().c_str(),
            r.error().code());
}

// ================================================================
// Constructor
// ================================================================
FirestoreHistory::FirestoreHistory()
    : _ready(false), _lastCleanupMs(0)
{
    _temp = { 0.0, 0, 0 };
    _ph   = { 0.0, 0, 0 };
    _tds  = { 0.0, 0, 0 };
    _flushCount  = 0;
    _lastFlushTs = 0;
}

// ================================================================
// BEGIN — nhận void* để tránh khai báo FirebaseApp trong header
// ================================================================
void FirestoreHistory::begin(void* appPtr) {
    if (_ready) return;

    // Cast về đúng kiểu
    FirebaseApp& app = *reinterpret_cast<FirebaseApp*>(appPtr);

    // Không cần setInsecure() riêng — dùng chung SSL context với RTDB
    app.getApp<Firestore::Documents>(Docs);

    _ready = true;
    LOG_INFO("FSH", "Ready. project=%s", FIRESTORE_PROJECT_ID);
}

// ================================================================
// LOOP
// ================================================================
void FirestoreHistory::loop() {
    if (!_ready) return;
    Docs.loop();
}

// ================================================================
// INGEST
// ================================================================
void FirestoreHistory::ingest(float temp, float ph, float tds) {
    if (!_ready) return;

    if (isnan(temp) || isnan(ph) || isnan(tds))  return;
    if (temp < -50.0f || temp > 100.0f)           return;
    if (ph   <   0.0f || ph   >  14.0f)           return;
    if (tds  <   0.0f || tds  > 9999.0f)          return;

    uint64_t epochMs = _epochMs();
    if (epochMs == 0) return;

    uint64_t bucket = _bucketFloor(epochMs);

    // Khởi tạo lần đầu
    if (_temp.bucketStart == 0) {
        _temp.bucketStart = bucket;
        _ph.bucketStart   = bucket;
        _tds.bucketStart  = bucket;
    }

    // Phát hiện sang phút mới → flush
    if (bucket > _temp.bucketStart) {
        if (_temp.count > 0 && _ph.count > 0 && _tds.count > 0) {
            _flush(_temp.bucketStart);
        }
        _temp = { 0.0, 0, bucket };
        _ph   = { 0.0, 0, bucket };
        _tds  = { 0.0, 0, bucket };
    }

    // Tích lũy
    _temp.sum += temp;  _temp.count++;
    _ph.sum   += ph;    _ph.count++;
    _tds.sum  += tds;   _tds.count++;
}

// ================================================================
// FLUSH
// ================================================================
void FirestoreHistory::_flush(uint64_t bucketMs) {
    float avgTemp = roundf((float)(_temp.sum / _temp.count) * 10.0f)  / 10.0f;
    float avgPh   = roundf((float)(_ph.sum   / _ph.count)  * 100.0f) / 100.0f;
    float avgTds  = roundf((float)(_tds.sum  / _tds.count));

    char docId[20];
    snprintf(docId, sizeof(docId), "%llu", (unsigned long long)bucketMs);

    _flushCount++;
    _lastFlushTs = (uint32_t)(bucketMs / 1000ULL);

    LOG_INFO("FSH", "Flush #%lu t=%llu T=%.1f pH=%.2f TDS=%.0f n=%d",
        (unsigned long)_flushCount,
        (unsigned long long)bucketMs, avgTemp, avgPh, avgTds, (int)_temp.count);

    _writeDoc("sensor_temp", docId, avgTemp, bucketMs);
    _writeDoc("sensor_ph",   docId, avgPh,   bucketMs);
    _writeDoc("sensor_tds",  docId, avgTds,  bucketMs);

    uint32_t now = millis();
    if (_lastCleanupMs == 0 || now - _lastCleanupMs > FSH_CLEANUP_MS) {
        _lastCleanupMs = now;
        _cleanup();
    }
}

// ================================================================
// WRITE DOC — dùng createDocument (tạo mới) với docId cố định
// Firestore::Documents::patch signature thực tế:
//   patch(aClient, Parent, docPath, PatchDocumentOptions,
//         Document<Values::Value>, cb, uid)
// ================================================================
void FirestoreHistory::_writeDoc(const char* col, const char* docId,
                                  float value, uint64_t bucketMs) {
    // Timestamp ISO 8601 — bucketMs là uint64_t, chia 1000 ra giây
    time_t sec   = (time_t)(bucketMs / 1000ULL);
    struct tm* t = gmtime(&sec);
    char tsStr[28];
    strftime(tsStr, sizeof(tsStr), "%Y-%m-%dT%H:%M:%SZ", t);

    // Document path
    char docPath[64];
    snprintf(docPath, sizeof(docPath), "%s/%s", col, docId);

    // Build Firestore Document với Values đúng kiểu
    Values::StringValue deviceVal(FIREBASE_DEVICE);
    Values::TimestampValue tsVal(tsStr);
    Values::DoubleValue vVal(value);

    Document<Values::Value> doc("device", Values::Value(deviceVal));
    doc.add("ts", Values::Value(tsVal));
    doc.add("v",  Values::Value(vVal));

    // PatchDocumentOptions(updateMask, mask, currentDocument)
    // Cả 3 đều rỗng = update tất cả fields, không có precondition
    DocumentMask updateMask;
    DocumentMask mask;
    Precondition precond;
    PatchDocumentOptions patchOpts(updateMask, mask, precond);

    Docs.patch(getSharedFstoreClient(),
        Firestore::Parent(FIRESTORE_PROJECT_ID),
        String(docPath),
        patchOpts,
        doc,
        onFstoreResult,
        String(docId));

    LOG_DEBUG("FSH", "patch %s v=%.4f", docPath, (double)value);
}

// ================================================================
// CLEANUP — log only, Web client xử lý delete
// ================================================================
void FirestoreHistory::_cleanup() {
    uint64_t epochMs = _epochMs();
    if (epochMs < (uint64_t)FSH_WINDOW_MS) return;
    LOG_INFO("FSH", "Cleanup check — cutoff=%lu ms ago", (unsigned long)FSH_WINDOW_MS);
}

// ================================================================
// HELPERS
// ================================================================
uint64_t FirestoreHistory::_bucketFloor(uint64_t epochMs) {
    return (epochMs / (uint64_t)FSH_BUCKET_MS) * (uint64_t)FSH_BUCKET_MS;
}

uint64_t FirestoreHistory::_epochMs() {
    time_t now = time(nullptr);
    if (now < 1577836800L) return 0ULL; // NTP chưa sync
    return (uint64_t)now * 1000ULL;
}