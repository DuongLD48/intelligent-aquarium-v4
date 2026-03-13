#pragma once
#include "type_definitions.h"
#include "analytics.h"
#include "safety_core.h"
#include "water_change_manager.h"
#include "system_constants.h"
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

// ================================================================
// oled_display.h
// Intelligent Aquarium v4.0
//
// SSD1306 128×64, giao tiếp SPI phần cứng (Adafruit library)
// 4 trang hiển thị, render throttle 500ms.
//
// Trang:
//   PAGE_SENSORS   — T, pH, TDS + source/status indicators
//   PAGE_ANALYTICS — EMA values, WSI bar, FSI bar, Drift
//   PAGE_RELAY     — 6 relay trạng thái ON/OFF
//   PAGE_SYSTEM    — Uptime, heap, WiFi RSSI, safe mode
// ================================================================

// ----------------------------------------------------------------
// PAGE ENUM
// ----------------------------------------------------------------
enum class OledPage : uint8_t {
    PAGE_SENSORS   = 0,
    PAGE_ANALYTICS = 1,
    PAGE_RELAY     = 2,
    PAGE_SYSTEM    = 3,
    PAGE_COUNT     = 4
};

// ----------------------------------------------------------------
// OLED DISPLAY
// ----------------------------------------------------------------
class OledDisplay {
public:
    OledDisplay();

    // Gọi trong setup(): khởi tạo SSD1306, vẽ splash screen
    bool begin();

    // Gọi trong loop() — render throttle 500ms
    void update(
        const CleanReading&    clean,
        const AnalyticsResult& aResult,
        const RelayCommand&    relayState,
        WaterChangeState       wcState,
        bool                   wifiConnected,
        bool                   safeMode
    );

    // Chuyển trang (từ button PAGE)
    void nextPage();
    void setPage(OledPage p);
    OledPage currentPage() const { return _page; }

private:
    Adafruit_SSD1306 _display;
    OledPage         _page;
    uint32_t         _lastRenderMs;

    static constexpr uint32_t RENDER_INTERVAL_MS = 500;

    // Render từng trang
    void _renderSensors  (const CleanReading& clean, WaterChangeState wcState);
    void _renderAnalytics(const AnalyticsResult& r);
    void _renderRelay    (const RelayCommand& cmd, WaterChangeState wcState);
    void _renderSystem   (bool wifiConnected, bool safeMode);

    // Helpers
    // Vẽ thanh progress ngang: x,y = góc trên trái, w=width, h=height, pct=0..100
    void _drawBar(int16_t x, int16_t y, int16_t w, int16_t h, float pct);

    // Indicator nguồn data: 'M'=MEASURED, 'L'=LAST, 'm'=MEDIAN, '!'=DEFAULT
    char _sourceChar(DataSource src) const;

    // Indicator status: ' '=OK, '?'=outlier, 'X'=error, 'R'=range
    char _statusChar(FieldStatus st) const;

    // Tên DriftDir ngắn gọn
    const char* _driftStr(DriftDir d) const;
};

// Global singleton
extern OledDisplay oledDisplay;
