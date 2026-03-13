#include "oled_display.h"
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
// ================================================================

// Global singleton
OledDisplay oledDisplay;

// SSD1306 SPI constructor: width, height, mosi, clk, dc, rst, cs
OledDisplay::OledDisplay()
    : _display(128, 64,
               PIN_OLED_MOSI, PIN_OLED_CLK,
               PIN_OLED_DC,   PIN_OLED_RES, PIN_OLED_CS),
      _page(OledPage::PAGE_SENSORS),
      _lastRenderMs(0)
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
    _display.setCursor(20, 20);
    _display.print("Intelligent Aquarium");
    _display.setCursor(40, 36);
    _display.print("v4.0  booting...");
    _display.display();
    delay(1500);

    _display.clearDisplay();
    _display.display();

    LOG_INFO("OLED", "SSD1306 init OK");
    return true;
}

// ================================================================
// UPDATE — render throttle 500ms
// ================================================================
void OledDisplay::update(
    const CleanReading&    clean,
    const AnalyticsResult& aResult,
    const RelayCommand&    relayState,
    WaterChangeState       wcState,
    bool                   wifiConnected,
    bool                   safeMode
) {
    uint32_t now = millis();
    if (now - _lastRenderMs < RENDER_INTERVAL_MS) return;
    _lastRenderMs = now;

    _display.clearDisplay();

    // Header: tên trang + trang số
    _display.setTextSize(1);
    _display.setCursor(0, 0);
    switch (_page) {
        case OledPage::PAGE_SENSORS:   _display.print("SENSORS");   break;
        case OledPage::PAGE_ANALYTICS: _display.print("ANALYTICS"); break;
        case OledPage::PAGE_RELAY:     _display.print("RELAYS");    break;
        case OledPage::PAGE_SYSTEM:    _display.print("SYSTEM");    break;
        default: break;
    }
    // Số trang ở góc phải: "1/4"
    char pgStr[6];
    snprintf(pgStr, sizeof(pgStr), "%d/4", (int)_page + 1);
    _display.setCursor(128 - strlen(pgStr) * 6, 0);
    _display.print(pgStr);

    // Đường kẻ ngang phân cách header
    _display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

    // Render nội dung trang
    switch (_page) {
        case OledPage::PAGE_SENSORS:
            _renderSensors(clean, wcState);
            break;
        case OledPage::PAGE_ANALYTICS:
            _renderAnalytics(aResult);
            break;
        case OledPage::PAGE_RELAY:
            _renderRelay(relayState, wcState);
            break;
        case OledPage::PAGE_SYSTEM:
            _renderSystem(wifiConnected, safeMode);
            break;
        default: break;
    }

    _display.display();
}

// ================================================================
// PAGE 1 — SENSORS
// Layout 128×54 (dưới header):
//   T:  25.4°C  [M]
//   pH:  7.02   [M]
//   TDS: 285ppm [M]
//   WATER: PUMPING_OUT   (hoặc IDLE)
// ================================================================
void OledDisplay::_renderSensors(const CleanReading& clean, WaterChangeState wcState) {
    _display.setTextSize(1);
    int16_t y = 12;
    char buf[32];

    // Nhiệt độ
    snprintf(buf, sizeof(buf), "T:  %5.1f%cC [%c%c]",
             clean.temperature, 0xF8,  // 0xF8 = degree symbol trong font Adafruit
             _sourceChar(clean.source_temperature),
             _statusChar(clean.status_temperature));
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // pH
    snprintf(buf, sizeof(buf), "pH: %5.2f   [%c%c]",
             clean.ph,
             _sourceChar(clean.source_ph),
             _statusChar(clean.status_ph));
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // TDS
    snprintf(buf, sizeof(buf), "TDS:%4.0fppm [%c%c]",
             clean.tds,
             _sourceChar(clean.source_tds),
             _statusChar(clean.status_tds));
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // Shock indicator
    if (clean.has_shock()) {
        _display.setCursor(0, y);
        _display.print("! SHOCK ");
        if (clean.shock_temperature) _display.print("T ");
        if (clean.shock_ph)          _display.print("pH");
        y += 10;
    }

    // Water change state
    _display.setCursor(0, y);
    _display.print("WC:");
    switch (wcState) {
        case WaterChangeState::IDLE:        _display.print("IDLE");         break;
        case WaterChangeState::PUMPING_OUT: _display.print("PUMP OUT >>"); break;
        case WaterChangeState::PUMPING_IN:  _display.print("PUMP IN  <<"); break;
        case WaterChangeState::DONE:        _display.print("DONE");         break;
    }
}

// ================================================================
// PAGE 2 — ANALYTICS
// Layout:
//   EMA T:25.4  pH:7.02
//   TDS:285
//   WSI:[==========] 87
//   FSI:[===       ] 12
//   Drift: T:UP pH:-- TDS:--
// ================================================================
void OledDisplay::_renderAnalytics(const AnalyticsResult& r) {
    _display.setTextSize(1);
    int16_t y = 12;
    char buf[32];

    // EMA values
    snprintf(buf, sizeof(buf), "EMA T:%.1f pH:%.2f", r.ema_temp, r.ema_ph);
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    snprintf(buf, sizeof(buf), "    TDS:%.0f ppm", r.ema_tds);
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // WSI bar
    _display.setCursor(0, y);
    _display.print("WSI:");
    _drawBar(24, y, 80, 7, r.wsi);
    snprintf(buf, sizeof(buf), "%3.0f", r.wsi);
    _display.setCursor(108, y); _display.print(buf);
    y += 10;

    // FSI bar (clamp hiển thị tối đa 100)
    float fsiDisplay = r.fsi > 100.0f ? 100.0f : r.fsi;
    _display.setCursor(0, y);
    _display.print("FSI:");
    _drawBar(24, y, 80, 7, fsiDisplay);
    snprintf(buf, sizeof(buf), "%3.0f", r.fsi);
    _display.setCursor(108, y); _display.print(buf);
    y += 10;

    // Drift status
    snprintf(buf, sizeof(buf), "Drift T:%s pH:%s TDS:%s",
             _driftStr(r.drift_temp),
             _driftStr(r.drift_ph),
             _driftStr(r.drift_tds));
    _display.setCursor(0, y); _display.print(buf);
}

// ================================================================
// PAGE 3 — RELAY
// Layout: 2 cột × 3 hàng, mỗi relay có dot indicator
//   [●] HEAT  [○] COOL
//   [○] pH+   [○] pH-
//   [○] IN    [○] OUT
//   WC: PUMPING_OUT
// ================================================================
void OledDisplay::_renderRelay(const RelayCommand& cmd, WaterChangeState wcState) {
    _display.setTextSize(1);
    int16_t y = 12;

    // Helper lambda-style: vẽ 1 relay cell
    auto drawRelay = [&](int16_t x, int16_t ry, bool on, const char* label) {
        if (on) {
            _display.fillCircle(x + 3, ry + 3, 3, SSD1306_WHITE);   // Dot đặc = ON
        } else {
            _display.drawCircle(x + 3, ry + 3, 3, SSD1306_WHITE);   // Dot rỗng = OFF
        }
        _display.setCursor(x + 9, ry);
        _display.print(label);
    };

    drawRelay(0,  y,      cmd.heater,   "HEAT");
    drawRelay(64, y,      cmd.cooler,   "COOL");
    y += 12;

    drawRelay(0,  y,      cmd.ph_up,    "pH+ ");
    drawRelay(64, y,      cmd.ph_down,  "pH- ");
    y += 12;

    drawRelay(0,  y,      cmd.pump_in,  "IN  ");
    drawRelay(64, y,      cmd.pump_out, "OUT ");
    y += 12;

    // Water change state
    _display.setCursor(0, y);
    _display.print("WC:");
    switch (wcState) {
        case WaterChangeState::IDLE:        _display.print("idle");      break;
        case WaterChangeState::PUMPING_OUT: _display.print("pump-out"); break;
        case WaterChangeState::PUMPING_IN:  _display.print("pump-in");  break;
        case WaterChangeState::DONE:        _display.print("done");      break;
    }
}

// ================================================================
// PAGE 4 — SYSTEM
// Layout:
//   Up: 2d 04:32:15
//   Heap: 218432 B
//   WiFi: OK (-72dBm)
//   Mode: NORMAL
// ================================================================
void OledDisplay::_renderSystem(bool wifiConnected, bool safeMode) {
    _display.setTextSize(1);
    int16_t y = 12;
    char buf[32];

    // Uptime
    uint32_t upSec  = millis() / 1000;
    uint32_t upDay  = upSec / 86400; upSec %= 86400;
    uint32_t upHour = upSec / 3600;  upSec %= 3600;
    uint32_t upMin  = upSec / 60;    upSec %= 60;
    snprintf(buf, sizeof(buf), "Up: %lud %02lu:%02lu:%02lu",
             (unsigned long)upDay, (unsigned long)upHour,
             (unsigned long)upMin, (unsigned long)upSec);
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // Heap
    snprintf(buf, sizeof(buf), "Heap: %lu B", (unsigned long)ESP.getFreeHeap());
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // WiFi
    if (wifiConnected) {
        snprintf(buf, sizeof(buf), "WiFi: OK (%ddBm)", WiFi.RSSI());
    } else {
        snprintf(buf, sizeof(buf), "WiFi: --");
    }
    _display.setCursor(0, y); _display.print(buf);
    y += 10;

    // System mode
    _display.setCursor(0, y);
    if (safeMode) {
        _display.print("Mode: !! SAFE MODE !!");
    } else {
        _display.print("Mode: NORMAL");
    }
}

// ================================================================
// HELPERS
// ================================================================

void OledDisplay::nextPage() {
    uint8_t next = ((uint8_t)_page + 1) % (uint8_t)OledPage::PAGE_COUNT;
    _page = (OledPage)next;
    LOG_DEBUG("OLED", "Page → %d", (int)_page);
}

void OledDisplay::setPage(OledPage p) {
    _page = p;
}

// Thanh progress ngang
void OledDisplay::_drawBar(int16_t x, int16_t y, int16_t w, int16_t h, float pct) {
    // Border
    _display.drawRect(x, y, w, h, SSD1306_WHITE);
    // Fill
    if (pct > 0.0f) {
        int16_t fillW = (int16_t)(pct / 100.0f * (float)(w - 2));
        if (fillW < 0)   fillW = 0;
        if (fillW > w-2) fillW = w - 2;
        if (fillW > 0) {
            _display.fillRect(x + 1, y + 1, fillW, h - 2, SSD1306_WHITE);
        }
    }
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
