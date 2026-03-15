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
// Điều hướng 4 nút kiểu menu điện thoại đời cũ:
//   UP / DOWN  → di chuyển con trỏ
//   SELECT     → vào / xác nhận
//   BACK       → quay lại màn trước
//
// ── Cấu trúc màn hình ──────────────────────────────────────────
//
//  [HOME] — màn hình chính, hiển thị tóm tắt nhanh
//     │
//     ├─ [INFO MENU] — chọn 1 trong 4 trang xem
//     │     ├─ 1. Sensors   (T, pH, TDS)
//     │     ├─ 2. Analytics (EMA, WSI, FSI, Drift)
//     │     ├─ 3. Relays    (6 relay ON/OFF)
//     │     └─ 4. System    (Uptime, Heap, WiFi)
//     │
//     └─ [ACTIONS MENU] — thao tác
//           └─ 1. Water Change  (xác nhận → kích hoạt thay nước)
//
// Điều hướng:
//   HOME     → UP/DOWN chọn mục → SELECT vào
//   INFO/xxx → UP/DOWN cuộn trang → BACK về HOME
//   ACTIONS  → UP/DOWN chọn action → SELECT xác nhận → BACK
// ================================================================

// ----------------------------------------------------------------
// UI STATE ENUM
// ----------------------------------------------------------------
enum class UiScreen : uint8_t {
    HOME            = 0,   // Màn hình chính (tóm tắt)
    INFO_MENU       = 1,   // Menu chọn trang xem
    VIEW_SENSORS    = 2,   // Xem T, pH, TDS
    VIEW_ANALYTICS  = 3,   // Xem EMA, WSI, FSI
    VIEW_RELAYS     = 4,   // Xem 6 relay
    VIEW_SYSTEM     = 5,   // Xem uptime, heap, wifi
    ACTIONS_MENU    = 6,   // Menu thao tác
    ACT_WATER_CHANGE = 7,  // Xác nhận thay nước thủ công
};

// ----------------------------------------------------------------
// OLED DISPLAY
// ----------------------------------------------------------------
class OledDisplay {
public:
    OledDisplay();

    bool begin();

    // Gọi trong loop() — render throttle 200ms
    void update(
        const CleanReading&    clean,
        const AnalyticsResult& aResult,
        const RelayCommand&    relayState,
        WaterChangeState       wcState,
        bool                   wifiConnected,
        bool                   safeMode
    );

    // Xử lý nút bấm — gọi sau buttonManager.update()
    // Trả true nếu action "kích hoạt thay nước" được xác nhận
    bool handleButtons();

    UiScreen currentScreen() const { return _screen; }

private:
    Adafruit_SSD1306 _display;
    UiScreen         _screen;
    uint8_t          _cursor;       // Con trỏ menu hiện tại
    uint32_t         _lastRenderMs;
    bool             _wcConfirmPending; // Đang chờ xác nhận thay nước?

    // Dữ liệu cached để render
    const CleanReading*    _pClean    = nullptr;
    const AnalyticsResult* _pAnalytics = nullptr;
    const RelayCommand*    _pRelay    = nullptr;
    WaterChangeState       _wcState   = WaterChangeState::IDLE;
    bool                   _wifi      = false;
    bool                   _safeMode  = false;

    static constexpr uint32_t RENDER_INTERVAL_MS = 200;

    // ── Render functions ─────────────────────────────────────────
    void _renderHome();
    void _renderInfoMenu();
    void _renderViewSensors();
    void _renderViewAnalytics();
    void _renderViewRelays();
    void _renderViewSystem();
    void _renderActionsMenu();
    void _renderActWaterChange();

    // ── Helpers ──────────────────────────────────────────────────
    void _drawHeader(const char* title, const char* hint = nullptr);
    void _drawBar(int16_t x, int16_t y, int16_t w, int16_t h, float pct);
    void _drawMenuItem(int16_t y, bool selected, const char* label);
    char _sourceChar(DataSource src) const;
    char _statusChar(FieldStatus st) const;
    const char* _driftStr(DriftDir d) const;

    // ── Navigation ───────────────────────────────────────────────
    void _goTo(UiScreen s, uint8_t cursor = 0);
    void _clampCursor(uint8_t maxItems);
};

// Global singleton
extern OledDisplay oledDisplay;
