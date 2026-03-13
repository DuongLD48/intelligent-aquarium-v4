#pragma once
#include "data_pipeline.h"
#include "safety_core.h"
#include "logger.h"
#include <math.h>

// ================================================================
// test_pipeline.h
// Intelligent Aquarium v4.0
//
// Unit test framework nhẹ cho ESP32 (không dùng thư viện ngoài).
// Chạy bằng cách uncomment #define RUN_PIPELINE_TESTS trong main.cpp
// và gọi runAllPipelineTests() trong setup().
// ================================================================

// ----------------------------------------------------------------
// MINI TEST FRAMEWORK
// ----------------------------------------------------------------
static int _testPass = 0;
static int _testFail = 0;
static const char* _currentSuite = "";

#define TEST_SUITE(name) \
    do { _currentSuite = (name); \
         Serial.printf("\n--- Suite: %s ---\n", _currentSuite); } while(0)

#define EXPECT_EQ(label, actual, expected) \
    do { \
        bool _ok = ((actual) == (expected)); \
        if (_ok) { _testPass++; \
            Serial.printf("  [PASS] %s\n", label); \
        } else { _testFail++; \
            Serial.printf("  [FAIL] %s: got %d expected %d\n", \
                          label, (int)(actual), (int)(expected)); \
        } \
    } while(0)

#define EXPECT_TRUE(label, expr) \
    do { \
        bool _ok = (bool)(expr); \
        if (_ok) { _testPass++; Serial.printf("  [PASS] %s\n", label); } \
        else     { _testFail++; Serial.printf("  [FAIL] %s (got false)\n", label); } \
    } while(0)

#define EXPECT_FALSE(label, expr) \
    do { \
        bool _ok = !(bool)(expr); \
        if (_ok) { _testPass++; Serial.printf("  [PASS] %s\n", label); } \
        else     { _testFail++; Serial.printf("  [FAIL] %s (got true)\n", label); } \
    } while(0)

#define EXPECT_NEAR(label, actual, expected, tol) \
    do { \
        bool _ok = (fabsf((float)(actual) - (float)(expected)) <= (float)(tol)); \
        if (_ok) { _testPass++; \
            Serial.printf("  [PASS] %s (%.4f)\n", label, (float)(actual)); \
        } else { _testFail++; \
            Serial.printf("  [FAIL] %s: %.4f != %.4f (tol=%.4f)\n", \
                          label, (float)(actual), (float)(expected), (float)(tol)); \
        } \
    } while(0)

#define EXPECT_SOURCE(label, actual, expected) \
    EXPECT_EQ(label, (int)(actual), (int)(expected))

#define EXPECT_STATUS(label, actual, expected) \
    EXPECT_EQ(label, (int)(actual), (int)(expected))

// ----------------------------------------------------------------
// HELPER: Tạo SensorReading hợp lệ
// ----------------------------------------------------------------
static SensorReading makeReading(float temp, float ph, float tds,
                                  uint32_t ts = 0) {
    SensorReading r;
    r.timestamp   = ts;
    r.temperature = temp;
    r.ph          = ph;
    r.tds         = tds;
    return r;
}

// ----------------------------------------------------------------
// HELPER: Feed N mẫu vào pipeline để làm đầy validRawBuffer
// (bypass MAD thì cần >= 10 mẫu)
// ----------------------------------------------------------------
static void feedPipeline(DataPipeline& p, float temp, float ph, float tds,
                          int count) {
    for (int i = 0; i < count; i++) {
        p.process(makeReading(temp, ph, tds, (uint32_t)i));
    }
}

// ----------------------------------------------------------------
// FORWARD DECLARATIONS — định nghĩa trong testpipeline.cpp
// ----------------------------------------------------------------
void testSuite1_RangeGate();
void testSuite2_MadFilter();
void testSuite3_ShockDetection();
void testSuite4_FallbackTiers();
void testSuite5_StaleSensor();
void testSuite6_Regression();

// ----------------------------------------------------------------
// ENTRY POINT
// ----------------------------------------------------------------
void runAllPipelineTests() {
    _testPass = 0;
    _testFail = 0;

    Serial.println("\n========================================");
    Serial.println("   PIPELINE UNIT TESTS — v4.0");
    Serial.println("========================================");

    testSuite1_RangeGate();
    testSuite2_MadFilter();
    testSuite3_ShockDetection();
    testSuite4_FallbackTiers();
    testSuite5_StaleSensor();
    testSuite6_Regression();

    Serial.println("\n========================================");
    Serial.printf("  TOTAL: %d passed, %d failed\n", _testPass, _testFail);
    Serial.println("========================================\n");

    if (_testFail == 0) {
        Serial.println(">>> ALL TESTS PASSED <<<");
    } else {
        Serial.printf(">>> %d TEST(S) FAILED <<<\n", _testFail);
    }
}
