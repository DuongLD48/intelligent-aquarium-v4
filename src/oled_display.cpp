#include "oled_display.h"
#include "button_manager.h"
#include "ph_session_manager.h"
#include "logger.h"
#include <WiFi.h>
#include <Wire.h>
#include <Arduino.h>

// ================================================================
// oled_display.cpp
// Intelligent Aquarium v4.0
//
// SSD1306 128×64 SPI
// CLK→14  MOSI→26  RES→27  DC→15  CS→21
//
// Menu navigation 4 nút:
//   UP/DOWN = di chuyển  |  SELECT = vào/xác nhận  |  BACK = quay lại
// ================================================================

OledDisplay oledDisplay;

OledDisplay::OledDisplay()
    : _display(128, 64,
               PIN_OLED_MOSI, PIN_OLED_CLK,
               PIN_OLED_DC,   PIN_OLED_RES, PIN_OLED_CS),
      _screen(UiScreen::HOME),
      _cursor(0),
      _lastRenderMs(0),
      _wcConfirmPending(false)
{}

// ----------------------------------------------------------------
bool OledDisplay::begin() {
    if (!_display.begin(SSD1306_SWITCHCAPVCC)) {
        LOG_ERROR("OLED", "SSD1306 init failed!");
        return false;
    }
    _display.clearDisplay();
    _display.setTextColor(SSD1306_WHITE);
    _display.setTextSize(1);

    // Splash screen
    _display.setCursor(12, 18);
    _display.print("Intelligent Aquarium");
    _display.setCursor(40, 32);
    _display.print("v4.0  booting...");
    _display.setCursor(16, 48);
    _display.print("UP DN SEL BCK ready");
    _display.display();
    delay(1500);
    _display.clearDisplay();
    _display.display();

    LOG_INFO("OLED", "SSD1306 init OK — 4-button menu mode");
    return true;
}

// ================================================================
// handleButtons — gọi sau buttonManager.update() trong loop()
// Trả true nếu user xác nhận thay nước thủ công
// ================================================================
bool OledDisplay::handleButtons() {
    bool wcTriggered = false;

    switch (_screen) {

    // ── HOME ─────────────────────────────────────────────────────
    // cursor 0 = INFO, cursor 1 = ACTIONS
    case UiScreen::HOME:
        if (buttonManager.wasPressed(BtnId::UP))   { if (_cursor > 0) _cursor--; }
        if (buttonManager.wasPressed(BtnId::DOWN))  { if (_cursor < 1) _cursor++; }
        if (buttonManager.wasPressed(BtnId::SELECT)) {
            if (_cursor == 0) _goTo(UiScreen::INFO_MENU);
            else              _goTo(UiScreen::ACTIONS_MENU);
        }
        // BACK ở HOME không làm gì (đã ở root)
        break;

    // ── INFO MENU ────────────────────────────────────────────────
    // cursor 0..3 → 4 trang xem
    case UiScreen::INFO_MENU:
        if (buttonManager.wasPressed(BtnId::UP))   { if (_cursor > 0) _cursor--; }
        if (buttonManager.wasPressed(BtnId::DOWN))  { _clampCursor(4); _cursor++; _clampCursor(4); }
        if (buttonManager.wasPressed(BtnId::SELECT)) {
            switch (_cursor) {
                case 0: _goTo(UiScreen::VIEW_SENSORS);   break;
                case 1: _goTo(UiScreen::VIEW_ANALYTICS); break;
                case 2: _goTo(UiScreen::VIEW_RELAYS);    break;
                case 3: _goTo(UiScreen::VIEW_SYSTEM);    break;
            }
        }
        if (buttonManager.wasPressed(BtnId::BACK)) _goTo(UiScreen::HOME);
        break;

    // ── VIEW PAGES — UP/DOWN cuộn (nếu sau này thêm scroll) ─────
    case UiScreen::VIEW_SENSORS:
    case UiScreen::VIEW_ANALYTICS:
    case UiScreen::VIEW_RELAYS:
    case UiScreen::VIEW_SYSTEM:
        // UP/DOWN có thể dùng để chuyển trang nhanh
        if (buttonManager.wasPressed(BtnId::UP)) {
            uint8_t s = (uint8_t)_screen;
            if (s > (uint8_t)UiScreen::VIEW_SENSORS) _screen = (UiScreen)(s - 1);
        }
        if (buttonManager.wasPressed(BtnId::DOWN)) {
            uint8_t s = (uint8_t)_screen;
            if (s < (uint8_t)UiScreen::VIEW_SYSTEM) _screen = (UiScreen)(s + 1);
        }
        if (buttonManager.wasPressed(BtnId::BACK)) _goTo(UiScreen::INFO_MENU);
        if (buttonManager.wasPressed(BtnId::SELECT)) _goTo(UiScreen::INFO_MENU);
        break;

    // ── ACTIONS MENU ─────────────────────────────────────────────
    // cursor 0 = Water Change (có thể thêm action sau)
    case UiScreen::ACTIONS_MENU:
        if (buttonManager.wasPressed(BtnId::UP))   { if (_cursor > 0) _cursor--; }
        if (buttonManager.wasPressed(BtnId::DOWN))  { if (_cursor < 0) _cursor++; } // max khi thêm action
        if (buttonManager.wasPressed(BtnId::SELECT)) {
            if (_cursor == 0) _goTo(UiScreen::ACT_WATER_CHANGE);
        }
        if (buttonManager.wasPressed(BtnId::BACK)) _goTo(UiScreen::HOME);
        break;

    // ── ACT: WATER CHANGE — xác nhận 2 bước ─────────────────────
    //  cursor 0 = YES, cursor 1 = NO
    case UiScreen::ACT_WATER_CHANGE:
        if (buttonManager.wasPressed(BtnId::UP))   { if (_cursor > 0) _cursor--; }
        if (buttonManager.wasPressed(BtnId::DOWN))  { if (_cursor < 1) _cursor++; }
        if (buttonManager.wasPressed(BtnId::SELECT)) {
            if (_cursor == 0) {
                wcTriggered = true;
                LOG_INFO("OLED", "Water change confirmed via menu");
            }
            _goTo(UiScreen::HOME);  // về HOME sau xác nhận hoặc hủy
        }
        if (buttonManager.wasPressed(BtnId::BACK)) _goTo(UiScreen::ACTIONS_MENU);
        break;

    default:
        _goTo(UiScreen::HOME);
        break;
    }

    return wcTriggered;
}

// ================================================================
// UPDATE — render throttle 200ms
// ================================================================
void OledDisplay::update(
    const CleanReading&    clean,
    const AnalyticsResult& aResult,
    const RelayCommand&    relayState,
    WaterChangeState       wcState,
    bool                   wifiConnected,
    bool                   safeMode
) {
    // Cache dữ liệu mới nhất
    _pClean    = &clean;
    _pAnalytics = &aResult;
    _pRelay    = &relayState;
    _wcState   = wcState;
    _wifi      = wifiConnected;
    _safeMode  = safeMode;

    uint32_t now = millis();
    if (now - _lastRenderMs < RENDER_INTERVAL_MS) return;
    _lastRenderMs = now;

    _display.clearDisplay();
    _display.setTextSize(1);

    switch (_screen) {
        case UiScreen::HOME:             _renderHome();           break;
        case UiScreen::INFO_MENU:        _renderInfoMenu();       break;
        case UiScreen::VIEW_SENSORS:     _renderViewSensors();    break;
        case UiScreen::VIEW_ANALYTICS:   _renderViewAnalytics();  break;
        case UiScreen::VIEW_RELAYS:      _renderViewRelays();     break;
        case UiScreen::VIEW_SYSTEM:      _renderViewSystem();     break;
        case UiScreen::ACTIONS_MENU:     _renderActionsMenu();    break;
        case UiScreen::ACT_WATER_CHANGE: _renderActWaterChange(); break;
        default: _renderHome(); break;
    }

    _display.display();
}

// ================================================================
// ── RENDER FUNCTIONS ─────────────────────────────────────────────
// ================================================================

// ----------------------------------------------------------------
// HOME — tóm tắt nhanh + chọn INFO / ACTIONS
// ┌────────────────────────┐
// │ AQUARIUM v4  [WiFi]    │
// ├────────────────────────┤
// │ T:26.2 pH:7.02         │
// │ TDS:285 WC:IDLE        │
// ├────────────────────────┤
// │ > [INFO]               │
// │   [ACTIONS]            │
// └────────────────────────┘
// ----------------------------------------------------------------
void OledDisplay::_renderHome() {
    // Header
    _display.setCursor(0, 0);
    _display.print("AQUARIUM v4");
    if (_wifi) {
        _display.setCursor(96, 0);
        _display.print("WiFi");
    }
    _display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

    // Sensor summary lines
    char buf[32];
    float lastPh = phSessionMgr.lastMedianPh();
    if (_pClean) {
        if (!isnan(lastPh)) {
            snprintf(buf, sizeof(buf), "T:%.1f pH:%.2f",
                     _pClean->temperature, lastPh);
        } else {
            snprintf(buf, sizeof(buf), "T:%.1f pH:--",
                     _pClean->temperature);
        }
    } else {
        if (!isnan(lastPh)) {
            snprintf(buf, sizeof(buf), "T:-- pH:%.2f", lastPh);
        } else {
            snprintf(buf, sizeof(buf), "T:-- pH:--");
        }
    }
    _display.setCursor(0, 12);
    _display.print(buf);

    if (_pClean) {
        snprintf(buf, sizeof(buf), "TDS:%.0f", _pClean->tds);
    } else {
        snprintf(buf, sizeof(buf), "TDS:--");
    }
    _display.setCursor(0, 22);
    _display.print(buf);

    _display.setCursor(58, 22);
    _display.print("WC:");
    switch (_wcState) {
        case WaterChangeState::IDLE:        _display.print("IDLE"); break;
        case WaterChangeState::PUMPING_OUT: _display.print("OUT");  break;
        case WaterChangeState::PUMPING_IN:  _display.print("IN");   break;
        case WaterChangeState::DONE:        _display.print("DONE"); break;
    }

    if (_safeMode) {
        _display.setCursor(88, 22);
        _display.print("SAFE");
    }

    _display.drawLine(0, 32, 127, 32, SSD1306_WHITE);

    // Menu items
    _drawMenuItem(35, _cursor == 0, "> INFO      (xem)");
    _drawMenuItem(47, _cursor == 1, "> ACTIONS   (thao tac)");

    // Footer hint
    _display.setCursor(0, 57);
    _display.print("UP/DN:chon  SEL:vao");
}

// ----------------------------------------------------------------
// INFO MENU — chọn 1 trong 4 trang xem
// ----------------------------------------------------------------
void OledDisplay::_renderInfoMenu() {
    _drawHeader("XEM THONG TIN", "SEL:vao  BCK:ve");

    _drawMenuItem(13, _cursor == 0, "1. Sensors  T/pH/TDS");
    _drawMenuItem(24, _cursor == 1, "2. Analytics WSI/FSI");
    _drawMenuItem(35, _cursor == 2, "3. Relay    6 relay");
    _drawMenuItem(46, _cursor == 3, "4. System   Heap/WiFi");
}

// ----------------------------------------------------------------
// VIEW SENSORS
// ----------------------------------------------------------------
void OledDisplay::_renderViewSensors() {
    _drawHeader("SENSORS  1/4", "UP/DN:trang  BCK:ve");

    if (!_pClean) return;
    const CleanReading& c = *_pClean;
    char buf[28];
    int16_t y = 13;

    snprintf(buf, sizeof(buf), "T:  %5.1f%cC  [%c%c]",
             c.temperature, 0xF8,
             _sourceChar(c.source_temperature),
             _statusChar(c.status_temperature));
    _display.setCursor(0, y); _display.print(buf); y += 10;

    // pH đến từ phSessionMgr (đo thưa, không qua pipeline)
    float lastPh = phSessionMgr.lastMedianPh();
    if (!isnan(lastPh)) {
        snprintf(buf, sizeof(buf), "pH: %5.2f [sess]", lastPh);
    } else {
        snprintf(buf, sizeof(buf), "pH: --    [sess]");
    }
    _display.setCursor(0, y); _display.print(buf); y += 10;

    snprintf(buf, sizeof(buf), "TDS:%4.0fppm  [%c%c]",
             c.tds,
             _sourceChar(c.source_tds),
             _statusChar(c.status_tds));
    _display.setCursor(0, y); _display.print(buf); y += 10;

    if (c.shock_temperature) {
        _display.setCursor(0, y);
        _display.print("! SHOCK: T");
    }
}

// ----------------------------------------------------------------
// VIEW ANALYTICS
// ----------------------------------------------------------------
void OledDisplay::_renderViewAnalytics() {
    _drawHeader("ANALYTICS  2/4", "UP/DN:trang  BCK:ve");

    if (!_pAnalytics) return;
    const AnalyticsResult& r = *_pAnalytics;
    char buf[28];
    int16_t y = 13;

    snprintf(buf, sizeof(buf), "EMA T:%.1f TDS:%.0f", r.ema_temp, r.ema_tds);
    _display.setCursor(0, y); _display.print(buf); y += 9;

    _display.setCursor(0, y); _display.print("WSI:");
    _drawBar(24, y, 76, 7, r.wsi);
    snprintf(buf, sizeof(buf), "%3.0f", r.wsi);
    _display.setCursor(104, y); _display.print(buf); y += 9;

    float fsiD = r.fsi > 100.0f ? 100.0f : r.fsi;
    _display.setCursor(0, y); _display.print("FSI:");
    _drawBar(24, y, 76, 7, fsiD);
    snprintf(buf, sizeof(buf), "%3.0f", r.fsi);
    _display.setCursor(104, y); _display.print(buf); y += 9;

    snprintf(buf, sizeof(buf), "T:%s TDS:%s",
             _driftStr(r.drift_temp), _driftStr(r.drift_tds));
    _display.setCursor(0, y); _display.print(buf);
}

// ----------------------------------------------------------------
// VIEW RELAYS
// ----------------------------------------------------------------
void OledDisplay::_renderViewRelays() {
    _drawHeader("RELAY  3/4", "UP/DN:trang  BCK:ve");

    if (!_pRelay) return;
    const RelayCommand& cmd = *_pRelay;
    int16_t y = 13;

    auto drawRelay = [&](int16_t x, int16_t ry, bool on, const char* label) {
        if (on) _display.fillCircle(x + 3, ry + 3, 3, SSD1306_WHITE);
        else    _display.drawCircle(x + 3, ry + 3, 3, SSD1306_WHITE);
        _display.setCursor(x + 9, ry);
        _display.print(label);
    };

    drawRelay(0,  y, cmd.heater,   "HEAT");
    drawRelay(64, y, cmd.cooler,   "COOL"); y += 12;
    drawRelay(0,  y, cmd.ph_up,    "pH+ ");
    drawRelay(64, y, cmd.ph_down,  "pH- "); y += 12;
    drawRelay(0,  y, cmd.pump_in,  "IN  ");
    drawRelay(64, y, cmd.pump_out, "OUT "); y += 12;

    _display.setCursor(0, y);
    _display.print("WC:");
    switch (_wcState) {
        case WaterChangeState::IDLE:        _display.print("idle");     break;
        case WaterChangeState::PUMPING_OUT: _display.print("pump-out"); break;
        case WaterChangeState::PUMPING_IN:  _display.print("pump-in");  break;
        case WaterChangeState::DONE:        _display.print("done");     break;
    }
}

// ----------------------------------------------------------------
// VIEW SYSTEM
// ----------------------------------------------------------------
void OledDisplay::_renderViewSystem() {
    _drawHeader("SYSTEM  4/4", "UP/DN:trang  BCK:ve");

    char buf[28];
    int16_t y = 13;

    uint32_t upSec  = millis() / 1000;
    uint32_t upDay  = upSec / 86400; upSec %= 86400;
    uint32_t upHr   = upSec / 3600;  upSec %= 3600;
    uint32_t upMin  = upSec / 60;    upSec %= 60;
    snprintf(buf, sizeof(buf), "Up:%lud %02lu:%02lu:%02lu",
             (unsigned long)upDay, (unsigned long)upHr,
             (unsigned long)upMin, (unsigned long)upSec);
    _display.setCursor(0, y); _display.print(buf); y += 10;

    snprintf(buf, sizeof(buf), "Heap: %lu B", (unsigned long)ESP.getFreeHeap());
    _display.setCursor(0, y); _display.print(buf); y += 10;

    if (_wifi) snprintf(buf, sizeof(buf), "WiFi: OK (%ddBm)", WiFi.RSSI());
    else       snprintf(buf, sizeof(buf), "WiFi: disconnected");
    _display.setCursor(0, y); _display.print(buf); y += 10;

    _display.setCursor(0, y);
    _display.print(_safeMode ? "Mode: !! SAFE MODE !!" : "Mode: NORMAL");
}

// ----------------------------------------------------------------
// ACTIONS MENU
// ----------------------------------------------------------------
void OledDisplay::_renderActionsMenu() {
    _drawHeader("THAO TAC", "SEL:chon  BCK:ve");

    _drawMenuItem(18, _cursor == 0, "> Thay nuoc thu cong");

    // Footer mô tả action được chọn
    _display.drawLine(0, 45, 127, 45, SSD1306_WHITE);
    _display.setCursor(0, 48);
    if (_cursor == 0) {
        switch (_wcState) {
            case WaterChangeState::IDLE:
                _display.print("WC: san sang"); break;
            case WaterChangeState::PUMPING_OUT:
            case WaterChangeState::PUMPING_IN:
                _display.print("WC: dang chay!"); break;
            case WaterChangeState::DONE:
                _display.print("WC: vua xong"); break;
        }
    }
}

// ----------------------------------------------------------------
// ACT: WATER CHANGE CONFIRM
// ┌────────────────────────┐
// │   THAY NUOC THU CONG   │
// ├────────────────────────┤
// │  Xac nhan bat dau?     │
// │                        │
// │  > [YES] Bat dau ngay  │
// │    [NO ] Huy bo        │
// └────────────────────────┘
// ----------------------------------------------------------------
void OledDisplay::_renderActWaterChange() {
    _drawHeader("THAY NUOC", "SEL:ok  BCK:huy");

    _display.setCursor(8, 14);
    _display.print("Xac nhan bat dau?");

    _display.drawLine(0, 24, 127, 24, SSD1306_WHITE);

    _drawMenuItem(27, _cursor == 0, "[YES] Bat dau ngay");
    _drawMenuItem(39, _cursor == 1, "[NO ] Huy bo");

    // Warning nếu WC đang chạy
    if (_wcState != WaterChangeState::IDLE &&
        _wcState != WaterChangeState::DONE) {
        _display.setCursor(0, 53);
        _display.print("! WC dang hoat dong");
    }
}

// ================================================================
// ── HELPERS ──────────────────────────────────────────────────────
// ================================================================

void OledDisplay::_drawHeader(const char* title, const char* hint) {
    _display.setCursor(0, 0);
    _display.print(title);
    if (hint) {
        // Hiển thị hint nhỏ ở dưới cùng
        _display.setCursor(0, 57);
        _display.print(hint);
    }
    _display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}

void OledDisplay::_drawMenuItem(int16_t y, bool selected, const char* label) {
    if (selected) {
        // Highlight: vẽ hình chữ nhật đặc làm nền
        _display.fillRect(0, y - 1, 128, 10, SSD1306_WHITE);
        _display.setTextColor(SSD1306_BLACK);
        _display.setCursor(2, y);
        _display.print(label);
        _display.setTextColor(SSD1306_WHITE);
    } else {
        _display.setCursor(2, y);
        _display.print(label);
    }
}

void OledDisplay::_drawBar(int16_t x, int16_t y, int16_t w, int16_t h, float pct) {
    _display.drawRect(x, y, w, h, SSD1306_WHITE);
    if (pct > 0.0f) {
        int16_t fillW = (int16_t)(pct / 100.0f * (float)(w - 2));
        if (fillW < 0)   fillW = 0;
        if (fillW > w-2) fillW = w - 2;
        if (fillW > 0)   _display.fillRect(x + 1, y + 1, fillW, h - 2, SSD1306_WHITE);
    }
}

void OledDisplay::_goTo(UiScreen s, uint8_t cursor) {
    LOG_DEBUG("OLED", "Screen %d → %d", (int)_screen, (int)s);
    _screen = s;
    _cursor = cursor;
}

void OledDisplay::_clampCursor(uint8_t maxItems) {
    if (_cursor >= maxItems) _cursor = maxItems - 1;
}

char OledDisplay::_sourceChar(DataSource src) const {
    switch (src) {
        case DataSource::MEASURED:         return 'M';
        case DataSource::FALLBACK_LAST:    return 'L';
        case DataSource::FALLBACK_MEDIAN:  return 'm';
        case DataSource::FALLBACK_DEFAULT: return '!';
        default:                           return '?';
    }
}

char OledDisplay::_statusChar(FieldStatus st) const {
    switch (st) {
        case FieldStatus::OK:            return ' ';
        case FieldStatus::OUT_OF_RANGE:  return 'R';
        case FieldStatus::MAD_OUTLIER:   return 'O';
        case FieldStatus::SENSOR_ERROR:  return 'X';
        default:                         return '?';
    }
}

const char* OledDisplay::_driftStr(DriftDir d) const {
    switch (d) {
        case DriftDir::UP:   return "UP ";
        case DriftDir::DOWN: return "DN ";
        default:             return "-- ";
    }
}
