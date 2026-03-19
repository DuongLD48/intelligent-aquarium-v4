#pragma once

// ================================================================
// firestore_history.h — Intelligent Aquarium v4.1
//
// Cách dùng:
//   1. wifi_firebase.h: thêm #define ENABLE_FIRESTORE
//   2. begin(): firestoreHistory.begin(app)
//   3. loop():  firestoreHistory.loop()
//   4. Sau upload: firestoreHistory.ingest(temp, ph, tds)
// ================================================================

// QUAN TRỌNG: KHÔNG include FirebaseClient.h ở đây.
// firestore_history.cpp sẽ include sau wifi_firebase.h.
// Header này chỉ dùng kiểu nguyên thủy để tránh conflict namespace.

#include <Arduino.h>

// Forward declaration — defined in wifi_firebase.cpp
// Trả về AsyncClientClass dùng chung với RTDB upload (tiết kiệm RAM)
class AsyncClientClass;
AsyncClientClass& getSharedFstoreClient();

// ── Timing ───────────────────────────────────────────────────────
#define FSH_BUCKET_MS      60000UL
#define FSH_WINDOW_MS      (12UL * 3600UL * 1000UL)
#define FSH_CLEANUP_MS     (30UL * 60UL * 1000UL)

// ================================================================
// FirestoreHistory
// begin() nhận void* để tránh phải khai báo FirebaseApp ở đây.
// Bên trong .cpp sẽ cast về firebase_ns::FirebaseApp&.
// ================================================================
class FirestoreHistory {
public:
    FirestoreHistory();

    // app truyền vào là firebase_ns::FirebaseApp& — cast trong .cpp
    void begin(void* appPtr);

    void loop();
    void ingest(float temp, float ph, float tds);

    bool isReady()      const { return _ready; }

    // Debug getters — trả về sample count của bucket hiện tại
    uint16_t getTempCount() const { return _temp.count; }
    uint16_t getPhCount()   const { return _ph.count;   }
    uint16_t getTdsCount()  const { return _tds.count;  }

    // Thời điểm bucket hiện tại (epoch ms)
    uint64_t getCurrentBucketMs() const { return _temp.bucketStart; }

    // Số giây đến flush tiếp theo
    uint32_t secondsUntilFlush() const {
        uint64_t epochMs = _epochMs();
        if (epochMs == 0 || _temp.bucketStart == 0) return 0;
        uint64_t nextBucket = _temp.bucketStart + FSH_BUCKET_MS;
        if (epochMs >= nextBucket) return 0;
        return (uint32_t)((nextBucket - epochMs) / 1000ULL);
    }

    // Tổng số lần flush thành công
    uint32_t getFlushCount() const { return _flushCount; }

    // Timestamp (epoch s) của lần flush cuối
    uint32_t getLastFlushTs() const { return _lastFlushTs; }

private:
    bool     _ready;

    struct SensorBuf {
        double   sum;
        uint16_t count;
        uint64_t bucketStart; // epoch ms — cần 64-bit tránh overflow
    };
    SensorBuf _temp, _ph, _tds;

    uint32_t _lastCleanupMs;
    uint32_t _flushCount;    // tổng số lần flush thành công
    uint32_t _lastFlushTs;   // epoch s lúc flush cuối

    static uint64_t _bucketFloor(uint64_t epochMs);
    void _flush(uint64_t bucketMs);
    void _writeDoc(const char* col, const char* docId,
                   float value, uint64_t bucketMs);
    void _cleanup();
    static uint64_t _epochMs();
};

extern FirestoreHistory firestoreHistory;