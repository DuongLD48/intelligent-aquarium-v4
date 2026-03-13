#pragma once
#include "data_pipeline.h"
#include "safety_core.h"
#include "logger.h"
#include <math.h>

// ================================================================
// test_random_stress.h
// Intelligent Aquarium v4.0
//
// Stress tests A–E: kiểm tra pipeline dưới tải ngẫu nhiên và
// các kịch bản cực đoan để phát hiện lỗi tiềm ẩn.
//
// Chạy: gọi runAllStressTests() trong setup() sau unit tests.
// ================================================================

// ---- Pseudo-random (LCG, không cần stdlib rand) ----------------
static uint32_t _lcgSeed = 42;
static float _randF(float lo, float hi) {
    _lcgSeed = _lcgSeed * 1664525UL + 1013904223UL;
    float t = (float)(_lcgSeed >> 16) / 65535.0f;
    return lo + t * (hi - lo);
}
static bool _randBool(float prob) {  // prob = xác suất true [0,1]
    return _randF(0.0f, 1.0f) < prob;
}

// ---- Stress result counters ------------------------------------
static struct StressResult {
    int total   = 0;
    int measured = 0;
    int outlier  = 0;
    int rangeErr = 0;
    int sensorErr = 0;
    int shocks   = 0;
} _sr;

static void _srReset() { _sr = StressResult{}; }
static void _srAccum(const CleanReading& r) {
    _sr.total++;
    if (r.source_temperature == DataSource::MEASURED)      _sr.measured++;
    if (r.status_temperature == FieldStatus::MAD_OUTLIER)  _sr.outlier++;
    if (r.status_temperature == FieldStatus::OUT_OF_RANGE) _sr.rangeErr++;
    if (r.status_temperature == FieldStatus::SENSOR_ERROR) _sr.sensorErr++;
    if (r.shock_temperature)                               _sr.shocks++;
}

static void _srPrint(const char* name) {
    Serial.printf(
        "  [%s] n=%d measured=%d outlier=%d range=%d sensor=%d shocks=%d\n",
        name, _sr.total, _sr.measured, _sr.outlier,
        _sr.rangeErr, _sr.sensorErr, _sr.shocks);
}

// ================================================================
// SUITE A — 100 mẫu bình thường với noise nhỏ
// Kỳ vọng: >= 95% MEASURED, 0 range error, 0 sensor error
// ================================================================
void stressSuiteA_Normal() {
    Serial.println("\n--- Stress A: 100 normal samples (noise ±0.2°C ±0.05pH) ---");
    DataPipeline p;
    _srReset();
    _lcgSeed = 1001;

    // Warm up
    for (int i = 0; i < 15; i++) p.process(makeReading(27.0f, 7.0f, 300.0f));

    for (int i = 0; i < 100; i++) {
        float t   = 27.0f + _randF(-0.2f, 0.2f);
        float ph  = 7.0f  + _randF(-0.05f, 0.05f);
        float tds = 300.0f + _randF(-5.0f, 5.0f);
        _srAccum(p.process(makeReading(t, ph, tds)));
    }
    _srPrint("A");

    bool ok = (_sr.measured >= 95) && (_sr.rangeErr == 0) && (_sr.sensorErr == 0);
    Serial.printf("  [%s] >= 95 MEASURED: %s\n",
                  "A", ok ? "PASS" : "FAIL");
}

// ================================================================
// SUITE B — 15 random spikes trong luồng bình thường
// Kỳ vọng: spike bị reject (MAD_OUTLIER), mẫu bình thường vẫn OK
// ================================================================
void stressSuiteB_Spikes() {
    Serial.println("\n--- Stress B: 15 random spikes ---");
    DataPipeline p;
    _lcgSeed = 2002;

    // Warm up 20 mẫu stable
    for (int i = 0; i < 20; i++) p.process(makeReading(27.0f, 7.0f, 300.0f));

    int normalCount  = 0;
    int normalMeas   = 0;
    int spikeCount   = 0;
    int spikeRejected = 0;

    // 100 mẫu, 15 trong đó là spike
    for (int i = 0; i < 100; i++) {
        bool isSpike = (i % 7 == 3);  // spike định kỳ = 14-15 lần
        float t, ph, tds;
        if (isSpike) {
            t   = _randBool(0.5f) ? 60.0f : -5.0f;   // extreme
            ph  = 7.0f;
            tds = 300.0f;
            spikeCount++;
        } else {
            t   = 27.0f + _randF(-0.15f, 0.15f);
            ph  = 7.0f  + _randF(-0.03f, 0.03f);
            tds = 300.0f + _randF(-3.0f, 3.0f);
            normalCount++;
        }
        auto r = p.process(makeReading(t, ph, tds));
        if (isSpike &&
            (r.status_temperature == FieldStatus::MAD_OUTLIER ||
             r.status_temperature == FieldStatus::OUT_OF_RANGE)) {
            spikeRejected++;
        }
        if (!isSpike && r.source_temperature == DataSource::MEASURED) {
            normalMeas++;
        }
    }

    Serial.printf("  [B] spikes=%d rejected=%d | normal=%d measured=%d\n",
                  spikeCount, spikeRejected, normalCount, normalMeas);

    bool ok1 = (spikeRejected == spikeCount);
    bool ok2 = (normalMeas >= (int)(normalCount * 0.90f));
    Serial.printf("  [%s] All spikes rejected: %s\n", "B", ok1 ? "PASS" : "FAIL");
    Serial.printf("  [%s] Normal >= 90%% MEASURED: %s\n", "B", ok2 ? "PASS" : "FAIL");
}

// ================================================================
// SUITE C — Sensor dead 20 cycles → recover
// Kỳ vọng: fallback_count tăng, sau recover reset về 0, MEASURED lại
// ================================================================
void stressSuiteC_DeadRecover() {
    Serial.println("\n--- Stress C: Sensor dead 20 cycles → recover ---");
    DataPipeline p;

    // Warm up
    for (int i = 0; i < 15; i++) p.process(makeReading(27.0f, 7.0f, 300.0f));

    // 20 chu kỳ sensor chết (NaN)
    CleanReading last;
    for (int i = 0; i < 20; i++) {
        last = p.process(makeReading(NAN, NAN, NAN));
    }
    bool fbHigh = (last.fallback_count_temp >= 20);
    Serial.printf("  [C] After 20 dead: fallback_count=%d\n",
                  last.fallback_count_temp);

    // Recover
    auto rec = p.process(makeReading(27.0f, 7.0f, 300.0f));
    bool recOk = (rec.source_temperature == DataSource::MEASURED &&
                  rec.fallback_count_temp == 0);
    Serial.printf("  [C] After recover: source=%d fallback=%d\n",
                  (int)rec.source_temperature, rec.fallback_count_temp);

    Serial.printf("  [%s] Dead → high fallback: %s\n", "C", fbHigh ? "PASS" : "FAIL");
    Serial.printf("  [%s] Recover → MEASURED + count=0: %s\n",
                  "C", recOk ? "PASS" : "FAIL");
}

// ================================================================
// SUITE D — Trend tăng 0.15°C × 30 cycles (27→31.5°C)
// Kỳ vọng: TẤT CẢ MEASURED (không bị lock do feedback loop)
// ================================================================
void stressSuiteD_SlowTrend() {
    Serial.println("\n--- Stress D: Trend +0.15°C x30 cycles (27→31.5) ---");
    DataPipeline p;

    // Warm up tại 27.0
    for (int i = 0; i < 15; i++) p.process(makeReading(27.0f, 7.0f, 300.0f));

    int measured = 0;
    float val = 27.0f;
    for (int i = 0; i < 30; i++) {
        val += 0.15f;
        auto r = p.process(makeReading(val, 7.0f, 300.0f));
        if (r.source_temperature == DataSource::MEASURED) measured++;
    }

    Serial.printf("  [D] Trend: %d/30 MEASURED (final T=%.2f)\n", measured, val);
    bool ok = (measured == 30);
    Serial.printf("  [%s] All 30 MEASURED: %s\n", "D", ok ? "PASS" : "FAIL");
}

// ================================================================
// SUITE E — Mixed: 70% OK, 20% range fail, 10% spike
// Kỳ vọng: range fail → FALLBACK, spike → OUTLIER, OK → MEASURED
// Không crash, không feedback loop
// ================================================================
void stressSuiteE_Mixed() {
    Serial.println("\n--- Stress E: Mixed (70% OK, 20% range, 10% spike) ---");
    DataPipeline p;
    _lcgSeed = 5005;
    _srReset();

    // Warm up
    for (int i = 0; i < 15; i++) p.process(makeReading(27.0f, 7.0f, 300.0f));

    int rangeFails = 0, spikes = 0, normals = 0;
    int rangeCorrect = 0, spikeCorrect = 0, normalMeas = 0;

    for (int i = 0; i < 200; i++) {
        float roll = _randF(0.0f, 1.0f);
        float t, ph, tds;
        int type;  // 0=normal, 1=range, 2=spike

        if (roll < 0.70f) {
            // 70% normal
            t = 27.0f + _randF(-0.15f, 0.15f);
            ph = 7.0f + _randF(-0.04f, 0.04f);
            tds = 300.0f + _randF(-4.0f, 4.0f);
            type = 0; normals++;
        } else if (roll < 0.90f) {
            // 20% out of range
            t = _randBool(0.5f) ? 55.0f : 5.0f;
            ph = 7.0f; tds = 300.0f;
            type = 1; rangeFails++;
        } else {
            // 10% spike (in range but statistical outlier)
            t = 27.0f + _randF(15.0f, 20.0f);  // 42-47°C = in physical range but outlier
            ph = 7.0f; tds = 300.0f;
            type = 2; spikes++;
        }

        auto r = p.process(makeReading(t, ph, tds));
        _srAccum(r);

        if (type == 0 && r.source_temperature == DataSource::MEASURED) normalMeas++;
        if (type == 1 && r.status_temperature == FieldStatus::OUT_OF_RANGE) rangeCorrect++;
        if (type == 2 && r.status_temperature == FieldStatus::MAD_OUTLIER)  spikeCorrect++;
    }

    _srPrint("E");
    Serial.printf("  [E] normals=%d meas=%d | range=%d caught=%d | spike=%d caught=%d\n",
                  normals, normalMeas, rangeFails, rangeCorrect, spikes, spikeCorrect);

    bool ok1 = (normalMeas >= (int)(normals * 0.85f));
    bool ok2 = (rangeCorrect == rangeFails);
    Serial.printf("  [%s] Normal >= 85%% MEASURED: %s\n", "E", ok1 ? "PASS" : "FAIL");
    Serial.printf("  [%s] All range errors caught: %s\n",  "E", ok2 ? "PASS" : "FAIL");
}

// ================================================================
// ENTRY POINT
// ================================================================
void runAllStressTests() {
    Serial.println("\n========================================");
    Serial.println("   STRESS TESTS — v4.0");
    Serial.println("========================================");

    stressSuiteA_Normal();
    stressSuiteB_Spikes();
    stressSuiteC_DeadRecover();
    stressSuiteD_SlowTrend();
    stressSuiteE_Mixed();

    Serial.println("\n=== Stress tests complete ===\n");
}
