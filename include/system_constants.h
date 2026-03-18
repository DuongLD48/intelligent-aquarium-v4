#pragma once

// ================================================================
// system_constants.h
// Intelligent Aquarium v4.0
// Tất cả hằng số hệ thống, pin definitions, calibration
// ================================================================

// ----------------------------------------------------------------
// SENSOR PINS
// ----------------------------------------------------------------
#define PIN_DS18B20     4   // OneWire — nhiệt độ DS18B20
#define PIN_PH_ADC      34  // ADC input-only — pH analog
#define PIN_TDS_ADC     35  // ADC input-only — TDS analog

// ----------------------------------------------------------------
// RELAY PINS (active LOW)
// ----------------------------------------------------------------
#define PIN_RELAY_HEATER    23  // Relay 1 — sưởi
#define PIN_RELAY_COOLER    19  // Relay 2 — làm mát
#define PIN_RELAY_PH_UP     18  // Relay 3 — bơm pH lên
#define PIN_RELAY_PH_DOWN   5   // Relay 4 — bơm pH xuống
#define PIN_RELAY_PUMP_IN   13  // Relay 5 — bơm nước vào
#define PIN_RELAY_PUMP_OUT  12  // Relay 6 — bơm nước ra

// ----------------------------------------------------------------
// OLED SSD1306 SPI PINS
// ----------------------------------------------------------------
#define PIN_OLED_CLK    14
#define PIN_OLED_MOSI   26
#define PIN_OLED_RES    27
#define PIN_OLED_DC     15
#define PIN_OLED_CS     21

// ----------------------------------------------------------------
// BUTTON PINS — 4 nút điều hướng (INPUT_PULLUP, nhấn = LOW)
// ↑↓ = di chuyển trong menu, SELECT = vào/xác nhận, BACK = quay lại
// ----------------------------------------------------------------
#define PIN_BTN_UP      33  // GPIO 33 — ↑ di chuyển lên
#define PIN_BTN_DOWN    32  // GPIO 32 — ↓ di chuyển xuống
#define PIN_BTN_SELECT  22  // GPIO 22 — vào menu / xác nhận
#define PIN_BTN_BACK    25  // GPIO 25 — quay lại màn hình trước

// ----------------------------------------------------------------
// PIN CONFLICT CHECK (tất cả các GPIO đang dùng)
//  4  → DS18B20 (OneWire)
//  5  → RELAY pH-
// 12  → RELAY PUMP_OUT
// 13  → RELAY PUMP_IN
// 14  → OLED CLK
// 15  → OLED DC
// 18  → RELAY pH+
// 19  → RELAY COOLER
// 21  → OLED CS
// 22  → BTN SELECT      ✓ no conflict
// 23  → RELAY HEATER
// 25  → BTN BACK        ✓ no conflict (PIN_BTN_PAGE đã xóa)
// 26  → OLED MOSI
// 27  → OLED RES
// 32  → BTN DOWN        ✓ no conflict
// 33  → BTN UP          ✓ no conflict
// 34  → pH ADC (input-only)
// 35  → TDS ADC (input-only)
// Đã giải phóng: GPIO 0 (BTN_BACK cũ), GPIO 2 (BTN_WATER_CHANGE cũ)
// ----------------------------------------------------------------

// ----------------------------------------------------------------
// SENSOR TIMING
// ----------------------------------------------------------------
#define SENSOR_READ_INTERVAL_MS   5000   // Chu kỳ đo: 5 giây
#define SENSOR_HISTORY_SIZE       120    // Lịch sử: 120 mẫu = 10 phút

// ----------------------------------------------------------------
// WATER CHANGE DEFAULTS
// ----------------------------------------------------------------
#define WATER_CHANGE_DEFAULT_PUMP_OUT_SEC   30
#define WATER_CHANGE_DEFAULT_PUMP_IN_SEC    60

// ----------------------------------------------------------------
// PH CALIBRATION
// ----------------------------------------------------------------
#define PH_CALIB_SLOPE    (-3.5f)
#define PH_CALIB_OFFSET   (14.35f)

// ----------------------------------------------------------------
// TDS CALIBRATION
// ----------------------------------------------------------------
#define TDS_CALIB_FACTOR  (1.0f)

// ----------------------------------------------------------------
// NTP / TIME
// ----------------------------------------------------------------
#define NTP_SERVER              "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC      25200   // UTC+7 (Việt Nam)
#define NTP_DAYLIGHT_OFFSET_SEC 0
