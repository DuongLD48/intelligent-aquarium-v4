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
// ※ PIN_TRIG (16) và PIN_ECHO (17) đã bỏ (HC-SR04 loại bỏ)

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
// BUTTON PINS (INPUT_PULLUP, nhấn = LOW)
// ----------------------------------------------------------------
#define PIN_BTN_PAGE          25
#define PIN_BTN_UP            33
#define PIN_BTN_DOWN          32
#define PIN_BTN_SELECT        22  // ĐỔI từ GPIO 15 — tránh trùng OLED DC
#define PIN_BTN_BACK          0   // Boot pin — cẩn thận
#define PIN_BTN_WATER_CHANGE  2   // Nút bấm thay nước thủ công

// ----------------------------------------------------------------
// SENSOR TIMING
// ----------------------------------------------------------------
#define SENSOR_READ_INTERVAL_MS   5000   // Chu kỳ đo: 5 giây
#define SENSOR_HISTORY_SIZE       120    // Lịch sử: 120 mẫu = 10 phút

// ----------------------------------------------------------------
// WATER CHANGE DEFAULTS
// ----------------------------------------------------------------
#define WATER_CHANGE_DEFAULT_PUMP_OUT_SEC   30   // Bơm ra mặc định 30 giây
#define WATER_CHANGE_DEFAULT_PUMP_IN_SEC    60   // Bơm vào mặc định 60 giây

// ----------------------------------------------------------------
// PH CALIBRATION
// ----------------------------------------------------------------
#define PH_CALIB_SLOPE    (-3.5f)   // slope voltage→pH
#define PH_CALIB_OFFSET   (2.50f)   // offset

// ----------------------------------------------------------------
// TDS CALIBRATION
// ----------------------------------------------------------------
#define TDS_CALIB_FACTOR  (1.0f)    // hệ số nhân TDS

// ----------------------------------------------------------------
// NTP / TIME
// ----------------------------------------------------------------
#define NTP_SERVER          "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC  25200   // UTC+7 (Việt Nam)
#define NTP_DAYLIGHT_OFFSET_SEC 0
