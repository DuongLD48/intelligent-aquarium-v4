#include "test_pipeline.h"
#include <math.h>

// ================================================================
// testpipeline.cpp
// Intelligent Aquarium v4.0
// 6 unit test suites cho DataPipeline + SafetyCore integration
// ================================================================

// ================================================================
// SUITE 1 — RANGE GATE
// ================================================================
void testSuite1_RangeGate() {
    TEST_SUITE("1: Range Gate");
    DataPipeline p;

    // 1.1 Giá trị hợp lệ → MEASURED
    {
        auto r = p.process(makeReading(27.0f, 7.0f, 300.0f));
        EXPECT_SOURCE("T in range → MEASURED",
                      r.source_temperature, DataSource::MEASURED);
        EXPECT_STATUS("T in range → OK",
                      r.status_temperature, FieldStatus::OK);
        EXPECT_NEAR("T value correct", r.temperature, 27.0f, 0.01f);
    }

    // 1.2 T out of range → FALLBACK_LAST (vì đã có reading trước)
    {
        auto r = p.process(makeReading(99.0f, 7.0f, 300.0f));
        EXPECT_SOURCE("T out of range → FALLBACK_LAST",
                      r.source_temperature, DataSource::FALLBACK_LAST);
        EXPECT_STATUS("T out of range → OUT_OF_RANGE",
                      r.status_temperature, FieldStatus::OUT_OF_RANGE);
        EXPECT_NEAR("T fallback = last good (27.0)", r.temperature, 27.0f, 0.01f);
    }

    // 1.3 NaN → SENSOR_ERROR
    {
        auto r = p.process(makeReading(NAN, 7.0f, 300.0f));
        EXPECT_STATUS("T NaN → SENSOR_ERROR",
                      r.status_temperature, FieldStatus::SENSOR_ERROR);
    }

    // 1.4 Boot + NaN → FALLBACK_DEFAULT (chưa có last_good)
    {
        DataPipeline fresh;
        auto r = fresh.process(makeReading(NAN, NAN, NAN));
        EXPECT_SOURCE("Boot NaN → FALLBACK_DEFAULT",
                      r.source_temperature, DataSource::FALLBACK_DEFAULT);
        // Default = midpoint [15,40] = 27.5
        EXPECT_NEAR("Boot NaN T = midpoint 27.5", r.temperature, 27.5f, 0.01f);
    }

    // 1.5 T dưới min → FALLBACK
    {
        DataPipeline fresh;
        fresh.process(makeReading(25.0f, 7.0f, 300.0f));
        auto r = fresh.process(makeReading(5.0f, 7.0f, 300.0f));  // < 15.0
        EXPECT_SOURCE("T below min → FALLBACK_LAST",
                      r.source_temperature, DataSource::FALLBACK_LAST);
    }
}

// ================================================================
// SUITE 2 — MAD FILTER
// ================================================================
void testSuite2_MadFilter() {
    TEST_SUITE("2: MAD Filter");
    DataPipeline p;

    // 2.1 Buffer < 10 → bypass MAD
    {
        // Feed 5 mẫu (< mad_min_samples=10)
        CleanReading last;
        for (int i = 0; i < 5; i++) {
            last = p.process(makeReading(27.0f, 7.0f, 300.0f));
        }
        EXPECT_SOURCE("Buffer < 10 → bypass MAD → MEASURED",
                      last.source_temperature, DataSource::MEASURED);
    }

    // 2.2 Spike 1 mẫu → MAD_OUTLIER, dùng median(validRaw)
    {
        DataPipeline p2;
        // Feed 15 mẫu stable tại 27.0
        feedPipeline(p2, 27.0f, 7.0f, 300.0f, 15);
        // Spike đột ngột
        auto r = p2.process(makeReading(45.0f, 7.0f, 300.0f));
        EXPECT_SOURCE("Spike → FALLBACK_MEDIAN",
                      r.source_temperature, DataSource::FALLBACK_MEDIAN);
        EXPECT_STATUS("Spike → MAD_OUTLIER",
                      r.status_temperature, FieldStatus::MAD_OUTLIER);
        // Fallback = median của validRaw ≈ 27.0
        EXPECT_NEAR("Spike fallback ≈ 27.0", r.temperature, 27.0f, 0.5f);
    }

    // 2.3 QUAN TRỌNG NHẤT: Trend tăng dần → MỌI mẫu MEASURED
    // Đây là test phân biệt "filter đọc validRaw" vs "filter đọc cleanBuffer"
    // Nếu filter đọc cleanBuffer → sẽ lock sau vài mẫu (feedback loop)
    // Nếu filter đọc validRaw  → tất cả accept vì median cũng tăng theo
    {
        DataPipeline p3;
        // Warm up 12 mẫu stable tại 27.0
        feedPipeline(p3, 27.0f, 7.0f, 300.0f, 12);

        // Tăng dần 0.2°C/chu kỳ (27.0 → 32.0 trong 25 chu kỳ)
        int measuredCount = 0;
        float val = 27.0f;
        for (int i = 0; i < 25; i++) {
            val += 0.2f;
            auto r = p3.process(makeReading(val, 7.0f, 300.0f));
            if (r.source_temperature == DataSource::MEASURED) measuredCount++;
        }
        // Tất cả phải là MEASURED (trend hợp lệ không phải spike)
        EXPECT_TRUE("Slow trend: all 25 MEASURED (no false locks)",
                    measuredCount == 25);
        Serial.printf("    Trend test: %d/25 MEASURED\n", measuredCount);
    }

    // 2.4 Spike rồi về → spike bị reject, giá trị tiếp theo accept
    {
        DataPipeline p4;
        feedPipeline(p4, 27.0f, 7.0f, 300.0f, 15);

        // Spike
        auto rSpike = p4.process(makeReading(50.0f, 7.0f, 300.0f));
        EXPECT_STATUS("Spike rejected",
                      rSpike.status_temperature, FieldStatus::MAD_OUTLIER);

        // Trở về bình thường
        auto rRecover = p4.process(makeReading(27.1f, 7.0f, 300.0f));
        EXPECT_SOURCE("After spike, normal → MEASURED",
                      rRecover.source_temperature, DataSource::MEASURED);
    }
}

// ================================================================
// SUITE 3 — SHOCK DETECTION
// ================================================================
void testSuite3_ShockDetection() {
    TEST_SUITE("3: Shock Detection");
    DataPipeline p;

    // Khởi tạo stable
    feedPipeline(p, 27.0f, 7.0f, 300.0f, 12);

    // 3.1 Delta lớn → shock flag = true, nhưng VẪN accept data (MEASURED)
    auto rShock = p.process(makeReading(31.5f, 7.0f, 300.0f));  // delta = 4.5 > 3.0
    EXPECT_TRUE("Large delta → shock_temperature = true",
                rShock.shock_temperature);
    EXPECT_SOURCE("Shock → vẫn MEASURED (không reject)",
                  rShock.source_temperature, DataSource::MEASURED);
    EXPECT_NEAR("Shock → giá trị giữ nguyên", rShock.temperature, 31.5f, 0.01f);

    // 3.2 Delta nhỏ → no shock
    feedPipeline(p, 27.0f, 7.0f, 300.0f, 12);
    auto rNormal = p.process(makeReading(27.2f, 7.0f, 300.0f));  // delta = 0.2 < 3.0
    EXPECT_FALSE("Small delta → no shock", rNormal.shock_temperature);

    // 3.3 pH shock
    DataPipeline p2;
    feedPipeline(p2, 27.0f, 7.0f, 300.0f, 12);
    auto rPhShock = p2.process(makeReading(27.0f, 7.8f, 300.0f));  // delta = 0.8 > 0.5
    EXPECT_TRUE("pH delta 0.8 → shock_ph = true", rPhShock.shock_ph);
    EXPECT_SOURCE("pH shock → still MEASURED", rPhShock.source_ph, DataSource::MEASURED);
}

// ================================================================
// SUITE 4 — FALLBACK TIERS
// ================================================================
void testSuite4_FallbackTiers() {
    TEST_SUITE("4: Fallback Tiers");

    // 4.1 Boot + bad data → FALLBACK_DEFAULT
    {
        DataPipeline p;
        auto r = p.process(makeReading(NAN, NAN, NAN));
        EXPECT_SOURCE("Boot NaN → DEFAULT", r.source_temperature, DataSource::FALLBACK_DEFAULT);
        EXPECT_SOURCE("Boot NaN pH → DEFAULT", r.source_ph, DataSource::FALLBACK_DEFAULT);
        EXPECT_SOURCE("Boot NaN TDS → DEFAULT", r.source_tds, DataSource::FALLBACK_DEFAULT);
    }

    // 4.2 FALLBACK_LAST: có prior reading, sau đó bad data
    {
        DataPipeline p;
        p.process(makeReading(27.0f, 7.0f, 300.0f));  // Lưu last_good
        auto r = p.process(makeReading(NAN, NAN, NAN));
        EXPECT_SOURCE("Has prior → FALLBACK_LAST", r.source_temperature, DataSource::FALLBACK_LAST);
        EXPECT_NEAR("FALLBACK_LAST = last good", r.temperature, 27.0f, 0.01f);
    }

    // 4.3 FALLBACK_MEDIAN: MAD reject sau khi có đủ buffer
    {
        DataPipeline p;
        feedPipeline(p, 27.0f, 7.0f, 300.0f, 15);
        auto r = p.process(makeReading(50.0f, 7.0f, 300.0f));  // Spike
        EXPECT_SOURCE("MAD reject → FALLBACK_MEDIAN",
                      r.source_temperature, DataSource::FALLBACK_MEDIAN);
        // Median ≈ 27.0 (buffer đầy mẫu 27.0)
        EXPECT_NEAR("FALLBACK_MEDIAN ≈ 27.0", r.temperature, 27.0f, 0.5f);
    }

    // 4.4 fallback_count tăng đúng
    {
        DataPipeline p;
        p.process(makeReading(27.0f, 7.0f, 300.0f));
        for (int i = 0; i < 3; i++) {
            p.process(makeReading(NAN, 7.0f, 300.0f));
        }
        auto r = p.process(makeReading(NAN, 7.0f, 300.0f));
        EXPECT_EQ("fallback_count_temp = 4", r.fallback_count_temp, 4);
    }

    // 4.5 fallback_count reset khi recover
    {
        DataPipeline p;
        p.process(makeReading(27.0f, 7.0f, 300.0f));
        for (int i = 0; i < 3; i++) p.process(makeReading(NAN, 7.0f, 300.0f));
        auto r = p.process(makeReading(27.0f, 7.0f, 300.0f));  // Recover
        EXPECT_EQ("fallback_count reset to 0 on recover", r.fallback_count_temp, 0);
    }
}

// ================================================================
// SUITE 5 — STALE SENSOR + SAFETY CORE INTEGRATION
// ================================================================
void testSuite5_StaleSensor() {
    TEST_SUITE("5: Stale Sensor + Safety Core");

    // 5.1 6 chu kỳ fallback → Safety tắt heater+cooler
    {
        DataPipeline p;
        SafetyCore safety;
        safety.begin();

        // Cho phép 1 mẫu tốt trước
        p.process(makeReading(27.0f, 7.0f, 300.0f));

        RelayCommand cmd;
        cmd.heater = true;  // Controller muốn bật heater

        // 6 chu kỳ NaN
        CleanReading last;
        for (int i = 0; i < 6; i++) {
            last = p.process(makeReading(NAN, 7.0f, 300.0f));
        }
        EXPECT_EQ("After 6 NaN: fallback_count_temp = 6",
                  last.fallback_count_temp, 6);

        SafetyEvent evt = safety.apply(cmd, last);
        EXPECT_FALSE("Stale temp → heater OFF", cmd.heater);
        EXPECT_FALSE("Stale temp → cooler OFF", cmd.cooler);
        EXPECT_EQ("Event = SENSOR_STALE", (int)evt, (int)SafetyEvent::SENSOR_STALE);
    }

    // 5.2 Sensor recover → relay hoạt động lại
    {
        DataPipeline p;
        SafetyCore safety;
        safety.begin();

        p.process(makeReading(27.0f, 7.0f, 300.0f));
        for (int i = 0; i < 6; i++) p.process(makeReading(NAN, 7.0f, 300.0f));

        // Recover
        auto clean = p.process(makeReading(27.0f, 7.0f, 300.0f));
        EXPECT_EQ("After recover: fallback_count_temp = 0",
                  clean.fallback_count_temp, 0);

        RelayCommand cmd; cmd.heater = true;
        SafetyEvent evt = safety.apply(cmd, clean);
        EXPECT_TRUE("After recover: heater allowed", cmd.heater);
        EXPECT_EQ("After recover: no STALE event", (int)evt, (int)SafetyEvent::NONE);
    }

    // 5.3 pH stale, temp OK → chỉ tắt pH pump, không ảnh hưởng heater
    {
        DataPipeline p;
        SafetyCore safety;
        safety.begin();

        p.process(makeReading(27.0f, 7.0f, 300.0f));
        for (int i = 0; i < 6; i++) {
            p.process(makeReading(27.0f, NAN, 300.0f));  // pH bad, temp good
        }
        auto clean = p.process(makeReading(27.0f, NAN, 300.0f));

        EXPECT_EQ("pH stale but temp_fallback = 0",
                  clean.fallback_count_temp, 0);
        EXPECT_EQ("pH fallback_count = 7",
                  clean.fallback_count_ph, 7);

        RelayCommand cmd;
        cmd.heater  = true;
        cmd.ph_up   = true;
        cmd.ph_down = false;
        safety.apply(cmd, clean);

        EXPECT_TRUE("pH stale: heater NOT affected", cmd.heater);
        EXPECT_FALSE("pH stale: ph_up OFF", cmd.ph_up);
    }
}

// ================================================================
// SUITE 7 — TEMPERATURE MAD LOCK & RECOVERY
// Cover các kịch bản: nhúng nước đá, rút ra, nhiễu DS18B20
// ================================================================
void testSuite7_TempMadLock() {
    TEST_SUITE("7: Temp MAD Lock & Recovery");

    // 7.1 Nhúng nước đá (29→25°C đột ngột): 9 lần đầu fallback, lần 10 force accept
    {
        DataPipeline p;
        feedPipeline(p, 29.0f, 7.0f, 300.0f, 15);  // Buffer đầy ~29°C

        int fallbackCount = 0;
        int measuredCount = 0;
        CleanReading last;
        for (int i = 0; i < 10; i++) {
            last = p.process(makeReading(25.0f, 7.0f, 300.0f));
            if (last.source_temperature == DataSource::FALLBACK_MEDIAN) fallbackCount++;
            if (last.source_temperature == DataSource::MEASURED)        measuredCount++;
        }
        // 9 lần đầu phải fallback
        EXPECT_EQ("Ice bath: 9 fallbacks before force accept", fallbackCount, 9);
        // Lần thứ 10 phải force accept
        EXPECT_SOURCE("Ice bath: lần 10 force accept → MEASURED",
                      last.source_temperature, DataSource::MEASURED);
        EXPECT_NEAR("Ice bath: force accept = 25.0°C", last.temperature, 25.0f, 0.1f);
    }

    // 7.2 Sau force accept, buffer kept → median dần dịch về giá trị mới
    // Không lock ngược chiều: các mẫu 25°C tiếp tục dần được accept
    {
        DataPipeline p;
        feedPipeline(p, 29.0f, 7.0f, 300.0f, 15);

        // Trigger force accept tại lần 10
        for (int i = 0; i < 10; i++) p.process(makeReading(25.0f, 7.0f, 300.0f));

        // Sau force accept: buffer chứa hỗn hợp 29°C và 25°C
        // → median dần dịch về 25°C → các mẫu 25°C ngày càng dễ pass
        // Feed thêm 20 mẫu 25°C: ít nhất nửa sau phải là MEASURED
        int measured = 0;
        for (int i = 0; i < 20; i++) {
            auto r = p.process(makeReading(25.0f + (float)(i % 3) * 0.05f, 7.0f, 300.0f));
            if (r.source_temperature == DataSource::MEASURED) measured++;
        }
        EXPECT_TRUE("After force accept: buffer converges, >= 10/20 MEASURED", measured >= 10);
        Serial.printf("    Post-recovery convergence: %d/20 MEASURED\n", measured);
    }

    // 7.3 Rút ra khỏi nước đá (25→29°C): phải lock lại 9 lần rồi force accept
    {
        DataPipeline p;
        feedPipeline(p, 29.0f, 7.0f, 300.0f, 15);
        // Trigger force accept về 25°C
        for (int i = 0; i < 10; i++) p.process(makeReading(25.0f, 7.0f, 300.0f));
        // Thêm mẫu ổn định ở 25°C để buffer đầy
        feedPipeline(p, 25.0f, 7.0f, 300.0f, 20);

        // Rút ra: lên lại 29°C
        int fallbacks = 0;
        CleanReading last;
        for (int i = 0; i < 10; i++) {
            last = p.process(makeReading(29.0f, 7.0f, 300.0f));
            if (last.source_temperature == DataSource::FALLBACK_MEDIAN) fallbacks++;
        }
        EXPECT_EQ("Pull out ice: 9 fallbacks then force accept", fallbacks, 9);
        EXPECT_SOURCE("Pull out ice: lần 10 = MEASURED",
                      last.source_temperature, DataSource::MEASURED);
        EXPECT_NEAR("Pull out ice: value = 29.0", last.temperature, 29.0f, 0.1f);
    }

    // 7.4 Spike ngắn 1 mẫu KHÔNG trigger force accept (fallback reset về 0)
    {
        DataPipeline p;
        feedPipeline(p, 29.0f, 7.0f, 300.0f, 15);

        // Spike 1 mẫu
        auto rSpike = p.process(makeReading(45.0f, 7.0f, 300.0f));
        EXPECT_SOURCE("Single spike → FALLBACK_MEDIAN", rSpike.source_temperature,
                      DataSource::FALLBACK_MEDIAN);
        EXPECT_EQ("Single spike: fallback_count = 1", rSpike.fallback_count_temp, 1);

        // Về bình thường → fallback_count phải reset
        auto rOk = p.process(makeReading(29.0f, 7.0f, 300.0f));
        EXPECT_SOURCE("After spike: normal → MEASURED", rOk.source_temperature,
                      DataSource::MEASURED);
        EXPECT_EQ("After spike: fallback_count reset = 0", rOk.fallback_count_temp, 0);
    }

    // 7.5 DS18B20 read failed liên tiếp (NaN) → FALLBACK_LAST, KHÔNG trigger force accept
    // (Range gate xử lý NaN trước MAD, fallback_count tăng riêng nhưng
    //  sau 10 lần thì safety SENSOR_STALE kick in chứ KHÔNG force accept giá trị NaN)
    {
        DataPipeline p;
        feedPipeline(p, 29.0f, 7.0f, 300.0f, 15);

        CleanReading last;
        for (int i = 0; i < 12; i++) {
            last = p.process(makeReading(NAN, 7.0f, 300.0f));
        }
        // NaN phải luôn FALLBACK_LAST (không bao giờ force accept NaN)
        EXPECT_SOURCE("12x NaN → still FALLBACK_LAST (not force accept)",
                      last.source_temperature, DataSource::FALLBACK_LAST);
        EXPECT_NEAR("12x NaN → fallback = last good 29.0", last.temperature, 29.0f, 0.1f);
        EXPECT_EQ("12x NaN: fallback_count = 12", last.fallback_count_temp, 12);
    }

    // 7.6 Trend giảm chậm (nước lạnh từ từ) → KHÔNG lock, MỌI mẫu MEASURED
    {
        DataPipeline p;
        feedPipeline(p, 29.0f, 7.0f, 300.0f, 15);

        int measured = 0;
        float val = 29.0f;
        for (int i = 0; i < 20; i++) {
            val -= 0.15f;  // -0.15°C/chu kỳ: 29→26°C trong 20 bước
            auto r = p.process(makeReading(val, 7.0f, 300.0f));
            if (r.source_temperature == DataSource::MEASURED) measured++;
        }
        EXPECT_TRUE("Slow cooling 29→26: all 20 MEASURED", measured == 20);
        Serial.printf("    Slow cooling: %d/20 MEASURED\n", measured);
    }
}

// ================================================================
// SUITE 6 — REGRESSION TESTS
// ================================================================
void testSuite6_Regression() {
    TEST_SUITE("6: Regression");

    // 6.1 Trend chậm 31→34 qua 20 chu kỳ → MỌI giá trị MEASURED
    // (verify không bị lock do trend được cập nhật trong validRaw)
    {
        DataPipeline p;
        feedPipeline(p, 31.0f, 7.0f, 300.0f, 15);  // Warm up

        int measured = 0;
        float val = 31.0f;
        for (int i = 0; i < 20; i++) {
            val += 0.15f;  // +0.15°C mỗi chu kỳ → 31 → 34
            auto r = p.process(makeReading(val, 7.0f, 300.0f));
            if (r.source_temperature == DataSource::MEASURED) measured++;
        }
        EXPECT_TRUE("Slow trend 31→34: all 20 MEASURED", measured == 20);
        Serial.printf("    Slow trend: %d/20 MEASURED\n", measured);
    }

    // 6.2 Sensor hư + fallback pH=8 → Safety cắt bơm sau 6 chu kỳ
    {
        DataPipeline p;
        SafetyCore safety;
        safety.begin();

        // Cho phép 1 mẫu tốt
        p.process(makeReading(27.0f, 7.0f, 300.0f));

        // pH hỏng → fallback về pH=7.0 (last_good)
        bool pumpCut = false;
        for (int i = 0; i < 8; i++) {
            auto clean = p.process(makeReading(27.0f, NAN, 300.0f));
            RelayCommand cmd; cmd.ph_up = true;
            safety.apply(cmd, clean);
            if (!cmd.ph_up && clean.fallback_count_ph >= 6) {
                pumpCut = true;
                Serial.printf("    pH pump cut at cycle %d (fb=%d)\n",
                              i+1, clean.fallback_count_ph);
                break;
            }
        }
        EXPECT_TRUE("pH pump cut after 6 stale cycles", pumpCut);
    }

    // 6.3 100 chu kỳ bình thường (noise nhỏ) → >= 98 MEASURED
    {
        DataPipeline p;
        feedPipeline(p, 27.0f, 7.0f, 300.0f, 15);

        int measured = 0;
        for (int i = 0; i < 100; i++) {
            // Noise nhỏ: ±0.1°C, ±0.02pH, ±2ppm
            float noise_t  = (float)(i % 3 - 1) * 0.10f;
            float noise_ph = (float)(i % 5 - 2) * 0.02f;
            float noise_tds = (float)(i % 7 - 3) * 2.0f;
            auto r = p.process(makeReading(27.0f + noise_t,
                                           7.0f  + noise_ph,
                                           300.0f + noise_tds));
            if (r.source_temperature == DataSource::MEASURED &&
                r.source_ph          == DataSource::MEASURED &&
                r.source_tds         == DataSource::MEASURED) {
                measured++;
            }
        }
        EXPECT_TRUE("100 normal cycles: >= 98 fully MEASURED", measured >= 98);
        Serial.printf("    Normal 100 cycles: %d/100 fully MEASURED\n", measured);
    }
}