# INTELLIGENT AQUARIUM — BẢN KẾ HOẠCH XÂY DỰNG TỪ ĐẦU
# Version: 6.0
# Ngày: 2026-03-12
# Mục tiêu: Xây lại toàn bộ firmware ESP32 từ zero
# Thay đổi v4.0:
#   - Loại bỏ cảm biến HC-SR04 và toàn bộ logic mực nước (water level)
#   - Thay thế bằng water_change_manager: thay nước theo lịch + nút bấm thủ công
#   - Luồng thay nước: pump out (X giây) → pump in (Y giây) → xong
#   - Sửa xung đột GPIO 15: OLED DC giữ GPIO 15, SELECT button đổi sang GPIO 22
# Thay đổi v5.0 (cập nhật theo code thực tế wifi_firebase):
#   - wifi_firebase: đổi từ 3-4 stream riêng lẻ → 1 unified stream tại node cha
#   - Upload interval: 30s → 5s (UPLOAD_INTERVAL_MS = 5000)
#   - FirebaseData objects: 5 → 2 (fbStream + fbUpload), tiết kiệm ~20-25KB heap
#   - BSSL buffers: stream 2048/512, upload 4096/1024
#   - Bước 17 (mở rộng stream) đã được tích hợp vào Bước 10
#   - Thêm heap guard (MIN_FREE_HEAP_BYTES = 20KB) bảo vệ OOM crash
# Thay đổi v6.0 (đơn giản hóa UI — Blynk-inspired):
#   - Phong cách: nền tối (#0f1117), card tối (#1a1d2e), không gradient phức tạp
#   - Màu accent: cam (nhiệt độ), xanh ngọc (pH), tím (TDS) — nhất quán toàn app
#   - Gauge: SVG arc đơn giản, số lớn dễ đọc, không animation nhiều bước
#   - Relay: toggle switch CSS thuần, READ-ONLY, layout 1 hàng icon+tên+toggle
#   - Water Change Panel: gọn 1 card ngang, progress bar chỉ hiện khi đang chạy
#   - Analytics: 2 gauge nhỏ (WSI/FSI) + 1 dòng drift text — không full-size chart
#   - Safety Log: bảng tối giản, dot màu + HH:MM + event name, max 10 dòng
#   - Settings pages: 1 cột trung tâm, card nối tiếp, footer sticky Save/Cancel
#   - Loại bỏ: dual-thumb slider, thanh pH gradient, EMA trend arrow, shadow nhiều lớp
#   - Logic Firebase (js/*.js) KHÔNG thay đổi — chỉ thay đổi HTML/CSS
#   - Thêm heap guard (MIN_FREE_HEAP_BYTES = 20KB) bảo vệ OOM crash
#   - Stream path: /devices/{ID}/ (node cha) thay vì 4 path con riêng lẻ


================================================================
MÔ TẢ DỰ ÁN
================================================================

Hệ thống bể cá thông minh chạy trên ESP32, tự động đo 3 thông
số (nhiệt độ, pH, TDS), lọc nhiễu, điều khiển 6 relay
(heater, cooler, bơm pH lên, bơm pH xuống, bơm nước vào, bơm
nước ra), hiển thị OLED, gửi dữ liệu lên Firebase.

Chức năng thay nước tự động KHÔNG dùng cảm biến mực nước:
- Theo lịch người dùng đặt trên Web Dashboard (so sánh NTP)
- Hoặc nhấn nút thay nước thủ công trên Dashboard / nút vật lý
- Luồng: pump out chạy X giây → pump in chạy Y giây → xong
  (X, Y do người dùng tự cấu hình)

Web Dashboard (HTML thuần) hiển thị real-time data, cho phép User
chỉnh thông số vận hành (nhiệt độ, pH, lịch thay nước), Admin chỉnh
thông số pipeline (range gate, MAD filter, shock detection) và
giới hạn an toàn (thermal cutoff, heater runtime, ...).
Giao tiếp qua Firebase Realtime Database.


================================================================
PHẦN CỨNG
================================================================

MCU: ESP32 DevKit V1

Cảm biến:
  - DS18B20 (nhiệt độ)   → GPIO 4 (OneWire)
  - pH analog             → GPIO 34 (ADC, input-only)
  - TDS analog            → GPIO 35 (ADC, input-only)
  ※ KHÔNG dùng cảm biến mực nước (đã loại bỏ HC-SR04)
  ※ GPIO 16, 17 giải phóng — có thể dùng cho mục đích khác

Relay module 6 kênh (active LOW):
  - Relay 1 (Heater)      → GPIO 23
  - Relay 2 (Cooler)      → GPIO 19
  - Relay 3 (pH Up)       → GPIO 18
  - Relay 4 (pH Down)     → GPIO 5
  - Relay 5 (Pump In)     → GPIO 13
  - Relay 6 (Pump Out)    → GPIO 12

OLED SSD1306 128x64 (SPI):
  - CLK  → GPIO 14
  - MOSI → GPIO 26
  - RES  → GPIO 27
  - DC   → GPIO 15
  - CS   → GPIO 21

Nút bấm (INPUT_PULLUP, nhấn = LOW):
  - PAGE        → GPIO 25
  - UP          → GPIO 33
  - DOWN        → GPIO 32
  - SELECT      → GPIO 22  ← ĐỔI từ GPIO 15 (tránh trùng OLED DC)
  - BACK        → GPIO 0   (boot pin, cẩn thận)
  - WATER_CHANGE→ GPIO 2   (nút bấm thay nước thủ công, tùy chọn)

Calibration:
  - pH: slope = -3.5, offset = 2.50
  - TDS: factor = 1.0

Chu kỳ đo: mỗi 5 giây
Buffer lịch sử: 120 mẫu = 10 phút

NTP Time Sync:
  - NTP server: pool.ntp.org
  - Timezone: UTC+7 (Việt Nam, GMT+25200)
  - Dùng để so sánh lịch thay nước tự động


================================================================
KIẾN TRÚC TỔNG THỂ — 9 MODULE
================================================================

Mỗi module = 1 cặp file .h + .cpp (trừ vài file header-only).
Dưới đây liệt kê theo thứ tự phụ thuộc (module trên không phụ
thuộc module dưới).

  ┌─────────────────────────────────────────────────────┐
  │                    main.cpp                         │
  │  Điều phối: đọc sensor → pipeline → control →      │
  │  safety → relay → analytics → display → firebase   │
  └──────────┬──────────────────────────────────────────┘
             │ gọi theo thứ tự
             ▼
  ┌──────────────────┐  ┌──────────────────┐
  │ system_manager   │  │ button_manager   │
  │ Boot, watchdog,  │  │ Đọc 6 nút bấm   │
  │ safe mode        │  │                  │
  └──────────────────┘  └──────────────────┘
             │
             ▼
  ┌──────────────────┐     ┌──────────────────┐
  │ sensor_manager   │     │ oled_display     │
  │ Đọc phần cứng,  │     │ 4 trang hiển thị │
  │ push rawBuffer   │     │                  │
  └────────┬─────────┘     └──────────────────┘
           │ SensorReading (T, pH, TDS)
           ▼
  ┌──────────────────────────────────────────┐
  │ data_pipeline  ← VIẾT MỚI HOÀN TOÀN     │
  │ Range → MAD(rawBuffer) → Shock flag      │
  │ Output: CleanReading (3 trường, bỏ water)│
  └────────┬─────────────────────────────────┘
           │ CleanReading
           ├────────────────────┐
           ▼                    ▼
  ┌──────────────────┐  ┌──────────────────┐
  │ hysteresis_ctrl  │  │ pid_controller   │
  │ Nhiệt (ON/OFF)  │  │ pH (PID pulse)   │
  │ deadband         │  │                  │
  └────────┬─────────┘  └────────┬─────────┘
           │ RelayCommand         │
           ├──────────────────────┘
           │
           ▼
  ┌──────────────────────────────────────────┐
  │ water_change_manager  ← MODULE MỚI       │
  │ Lịch tự động (NTP) + nút bấm thủ công   │
  │ Luồng: pump_out(Xs) → pump_in(Ys) → xong│
  │ Ghi relay_cmd pump_in / pump_out         │
  └────────┬─────────────────────────────────┘
           │ RelayCommand (merge)
           ▼
  ┌──────────────────────────────────────────┐
  │ safety_core                              │
  │ 7 check tuần tự, ghi relay vật lý       │
  │ (bỏ check water level / water stale)     │
  └──────────────────────────────────────────┘
           │
           ▼
  ┌──────────────────┐  ┌──────────────────┐
  │ analytics        │  │ wifi_firebase    │
  │ EMA, CUSUM,      │  │ Gửi telemetry,  │
  │ WSI, FSI         │  │ nhận config      │
  └──────────────────┘  └──────────────────┘

  Ngoài ra:
  - water_change_manager — MODULE MỚI: lịch thay nước + thủ công
  - config_manager    — NVS persistence, JSON parse
  - control_config    — struct ControlConfig + validator
  - type_definitions  — SensorReading, CleanReading, enums
  - system_constants  — pin definitions, calibration
  - circular_buffer   — template circular buffer
  - credentials       — WiFi/Firebase secrets
  - logger            — log levels, Serial output


================================================================
NGUYÊN TẮC THIẾT KẾ (rút kinh nghiệm từ v1)
================================================================

1. FILTER KHÔNG ĐỌC OUTPUT CỦA CHÍNH NÓ
   MAD filter đọc validRawBuffer (input gốc), không đọc
   cleanBuffer (output đã lọc). Tránh feedback loop tự khoá.

2. MỖI TẦNG 1 NHIỆM VỤ DUY NHẤT
   Range = loại rác. MAD = loại spike. Shock = chỉ cảnh báo.
   Không có tầng nào vừa loại vừa cảnh báo vừa pending.

3. ĐƠN GIẢN TỐI ĐA
   3 tầng filter thay vì 5. Bỏ Hold-and-Confirm.
   Ít state chia sẻ = ít bug tương tác.

4. FAIL-SAFE
   Nghi ngờ → tắt relay. Sensor hư → dừng điều khiển tương ứng.
   Không bao giờ điều khiển dựa trên data cũ quá lâu.

5. SENSOR ĐỘC LẬP
   pH hư → chỉ tắt pH pump. Nhiệt độ vẫn hoạt động bình thường.
   ※ THAY ĐỔI: Water level sensor đã bỏ → không còn rule
               "water hư → tắt tất cả". Thay nước được điều khiển
               hoàn toàn bởi timer, không phụ thuộc sensor.


================================================================
MAIN LOOP — LUỒNG CHẠY MỖI 5 GIÂY
================================================================

  1. buttonManager.update()         — đọc nút bấm (6 nút)
  2. systemManager.update()         — feed watchdog, check safe mode
  3. tickPhPulse()                  — tắt relay pH khi hết pulse
  4. waterChangeManager.update()    — kiểm tra lịch NTP, xử lý luồng pump
  5. configManagerEx.handleSerial() — nhận config từ Serial
  6. wifiManager.loop()             — reconnect WiFi
  7. firebaseClient.loop()          — readStream() xử lý config
                                      real-time qua unified stream + gửi telemetry mỗi 5s
  8. oledDisplay.update()           — render OLED mỗi 500ms
  9. readSensors()                  — đọc phần cứng → rawBuffer (T, pH, TDS)
  10. dataPipeline.process(raw)     — lọc → CleanReading
  11. cleanBuffer.push(clean)       — lưu lịch sử
  12. analytics.update(cleanBuffer) — EMA, CUSUM, WSI, FSI
  13. hysteresis.compute(clean)     — tính relay heater/cooler
  14. pid.compute(clean)            — tính relay pH
  15. waterChangeManager.getCmd()   — merge lệnh pump vào RelayCommand
  16. safetyCore.apply(cmd, clean)  — kiểm tra an toàn
  17. safetyCore.writeRelays(cmd)   — ghi GPIO
  18. debugPrintCycle()             — in Serial monitor


================================================================
THỨ TỰ THỰC HIỆN — 17 BƯỚC
================================================================

Mỗi bước = 1 đoạn chat riêng.
Mỗi bước output ra file hoàn chỉnh, có thể compile/chạy được.
Copy-paste bản kế hoạch này vào mỗi đoạn chat mới.

  Bước 1-12:  Firmware ESP32 (giữ nguyên, trừ các thay đổi v4.0 + v5.0)
  Bước 7.5:   Firmware — water_change_manager (MODULE MỚI, thêm vào sau bước 7)
  Bước 10:    WiFi + Firebase — UNIFIED STREAM (đã bao gồm nội dung Bước 17 cũ)
  Bước 13:    Web — Firebase Init + CSS chung
  Bước 14:    Web — Trang Dashboard (real-time monitor)
  Bước 15:    Web — Trang User Settings (bổ sung lịch thay nước)
  Bước 16:    Web — Trang Admin

────────────────────────────────────────────────
BƯỚC 1: NỀN TẢNG — Header-only files
────────────────────────────────────────────────

Tạo 4 file không có logic, chỉ định nghĩa:

1a. system_constants.h
    - Tất cả pin definitions (sensor, relay, OLED, button)
    - SENSOR_READ_INTERVAL_MS = 5000
    - SENSOR_HISTORY_SIZE = 120
    ※ LOẠI BỎ: MAX_WATER_LEVEL_CM, MAX_ULTRASONIC_RANGE_CM,
               PIN_TRIG (16), PIN_ECHO (17)
    ※ THÊM MỚI:
      PIN_BTN_SELECT = 22        (đổi từ 15, tránh trùng OLED DC)
      PIN_BTN_WATER_CHANGE = 2   (nút bấm thay nước thủ công)
      WATER_CHANGE_DEFAULT_PUMP_OUT_SEC = 30
      WATER_CHANGE_DEFAULT_PUMP_IN_SEC  = 60
    - Calibration: PH_CALIB_SLOPE = -3.5, PH_CALIB_OFFSET = 2.50
    - TDS_CALIB_FACTOR = 1.0
    - NTP_SERVER = "pool.ntp.org"
    - NTP_GMT_OFFSET_SEC = 25200  (UTC+7)

1b. type_definitions.h
    - enum DataSource: MEASURED, FALLBACK_LAST, FALLBACK_MEDIAN, FALLBACK_DEFAULT
    - enum FieldStatus: OK, OUT_OF_RANGE, MAD_OUTLIER, SENSOR_ERROR
    - struct SensorReading: timestamp, temperature, ph, tds
      ※ LOẠI BỎ: water_level_cm (không còn cảm biến mực nước)
    - struct CleanReading:
        timestamp
        temperature, ph, tds
        source_* (DataSource cho mỗi field — chỉ còn 3 field)
        status_* (FieldStatus cho mỗi field)
        shock_temperature, shock_ph (bool flags)
        fallback_count_temp, fallback_count_ph,
        fallback_count_tds (uint8_t)
        helpers: is_fully_clean(), has_shock()
    - enum WaterChangeState: IDLE, PUMPING_OUT, PUMPING_IN, DONE
    - struct WaterChangeSchedule:
        bool enabled
        uint8_t hour, minute       (giờ chạy mỗi ngày)
        uint16_t pump_out_sec      (thời gian bơm ra, giây)
        uint16_t pump_in_sec       (thời gian bơm vào, giây)
        uint32_t last_run_day      (epoch/86400, tránh chạy 2 lần/ngày)

1c. circular_buffer.h
    - Template CircularBuffer<T, Capacity>
    - push(), shift(), last(), operator[], size(), isEmpty(), isFull()

1d. credentials.h
    - WIFI_SSID, WIFI_PASSWORD
    - FIREBASE_URL, FIREBASE_DEVICE, FIREBASE_TOKEN

────────────────────────────────────────────────
BƯỚC 2: LOGGER
────────────────────────────────────────────────

Tạo 2 file:

2a. logger.h
    - Log levels: ERROR, WARNING, INFO, DEBUG, VERBOSE
    - Macros: LOG_ERROR(), LOG_WARNING(), LOG_INFO(), LOG_DEBUG(), LOG_VERBOSE()
    - Current level configurable

2b. logger.cpp
    - logger_init(): Serial.begin(115200)
    - Prefix mỗi dòng log: [level] message

────────────────────────────────────────────────
BƯỚC 3: SENSOR MANAGER
────────────────────────────────────────────────

Tạo 2 file:

3a. sensor_manager.h
    - extern rawSensorBuffer
    - sensor_manager_init(), readSensors(), isSensorDataReady()

3b. sensor_manager.cpp
    Trách nhiệm DUY NHẤT: đọc phần cứng, đẩy vào rawSensorBuffer.
    - DS18B20: requestTemperatures(), getTempCByIndex(0)
      Nếu DEVICE_DISCONNECTED_C hoặc NaN hoặc ngoài [-55, 125] → dùng last raw hoặc NaN
    - pH: analogRead(GPIO34) → voltage → pH (calibration)
    - TDS: analogRead(GPIO35) → voltage → TDS ppm (calibration + bù nhiệt)
    ※ LOẠI BỎ: Ultrasonic HC-SR04 (pulseIn, water_level_cm)
    KHÔNG có bất kỳ logic lọc nào ở đây.

────────────────────────────────────────────────
BƯỚC 4: DATA PIPELINE — PHẦN QUAN TRỌNG NHẤT
────────────────────────────────────────────────

Tạo 2 file:

4a. data_pipeline.h

  struct PipelineConfig {
      // Range limits (chỉ còn 3 cảm biến)
      float temp_min = 15.0f, temp_max = 40.0f;
      float ph_min = 4.0f, ph_max = 10.0f;
      float tds_min = 1.0f, tds_max = 3000.0f;
      ※ LOẠI BỎ: water_level_min, water_level_max

      // MAD filter
      size_t mad_window_size = 30;
      size_t mad_min_samples = 10;
      float mad_threshold = 3.5f;
      float mad_floor_temp = 0.30f;
      float mad_floor_ph = 0.08f;
      float mad_floor_tds = 3.0f;
      ※ LOẠI BỎ: mad_floor_water

      // Shock (chỉ flag, không reject)
      float shock_temp_delta = 3.0f;
      float shock_ph_delta = 0.5f;
  };

  class DataPipeline {
  public:
      CleanReading process(const SensorReading& raw);
      void reset();
      void setConfig(const PipelineConfig& cfg);
  private:
      // validRaw buffers: ring buffer float[30] cho mỗi field
      // Chỉ chứa giá trị ĐÃ QUA RANGE CHECK
      // MAD filter đọc từ đây (KHÔNG đọc cleanBuffer)
      // ...
  };

4b. data_pipeline.cpp

  Luồng xử lý MỖI FIELD (giống nhau cho cả 4):

  TẦNG 1 — RANGE GATE:
    if NaN hoặc ngoài [min, max]:
      → status = OUT_OF_RANGE
      → fallback_count++
      → giá trị = last_good (hoặc default nếu chưa có)
      → KHÔNG push vào validRawBuffer
      → xong field này

  TẦNG 2 — MAD FILTER (chỉ khi qua range):
    Push raw vào validRawBuffer trước
    if validRawBuffer.count < mad_min_samples (10):
      → bypass MAD, accept luôn
    else:
      → median = median(validRawBuffer)
      → MAD = median(|mỗi giá trị - median|)
      → if MAD < mad_floor: MAD = mad_floor
      → z = 0.6745 × |raw - median| / MAD
      → if z > 3.5: OUTLIER
          → status = MAD_OUTLIER
          → fallback_count++
          → giá trị = median(validRawBuffer)  ← KHÔNG PHẢI cleanBuffer
      → else: ACCEPT
          → status = OK, source = MEASURED
          → fallback_count = 0
          → cập nhật last_good = raw

  TẦNG 3 — SHOCK FLAG (chỉ cho temp và pH, sau khi accept):
    if source == MEASURED và has_last_good:
      if |giá_trị - last_good| > shock_threshold:
        → shock flag = true
    (vẫn giữ giá trị, không reject)

  Cuối cùng: ghi fallback_count vào CleanReading output.

  CHÚ Ý QUAN TRỌNG:
  - process() KHÔNG nhận cleanBuffer parameter
  - Pipeline tự quản lý validRawBuffer bên trong
  - validRawBuffer là ring buffer float[30], rất nhẹ
  - Range fail → KHÔNG push vào validRawBuffer (giữ buffer sạch)

────────────────────────────────────────────────
BƯỚC 5: CONTROL CONFIG + CONFIG MANAGER
────────────────────────────────────────────────

Tạo 4 file:

5a. control_config.h
    - struct ControlConfig: temp_min/max, ph_min/max, tds_target/tolerance,
      water_min/max_target, pid_kp/ki/kd
    - Helpers: tempTarget(), tempDeadband(), phSetpoint()
    - ConfigValidator: validate(), trả về ConfigError enum

5b. control_config.cpp
    - ConfigValidator::validate() kiểm tra range hợp lý

5c. config_manager.h
    - NVS persistence (ESP32 Preferences)
    - JSON parser (nhận từ Serial/Firebase)
    - loadFromNvs(), saveToNvs(), parseFromJson()
    - handleSerial() — đọc JSON từ Serial

5d. config_manager.cpp

────────────────────────────────────────────────
BƯỚC 6: SAFETY CORE
────────────────────────────────────────────────

Tạo 2 file:

6a. safety_core.h
    - HardLimits namespace:
        THERMAL_CUTOFF_C = 42.0
        TEMP_EMERGENCY_COOL_C = 38.0
        HEATER_MAX_RUNTIME_MS = 10 phút
        HEATER_COOLDOWN_MS = 5 phút
        PH_PUMP_MAX_PULSE_MS = 3 giây
        PH_PUMP_MIN_INTERVAL_MS = 30 giây
        ※ LOẠI BỎ: WATER_CRITICAL_LOW_CM = 3.0
        STALE_SENSOR_THRESHOLD = 6
    - RelayIndex enum (6 relay)
    - RelayCommand struct
    - SafetyEvent enum (10 events — bỏ water-related events)
    - SafetyCore class

6b. safety_core.cpp
    7 check tuần tự trong apply() (giảm từ 8, bỏ water level check):
    1. _checkSensorReliability — FALLBACK_DEFAULT → tắt relay tương ứng
    2. _checkStaleSensor — fallback_count >= 6 → tắt relay tương ứng
    3. ※ LOẠI BỎ: _checkWaterLevel (không còn cảm biến mực nước)
    3. _checkThermal — >= 42°C cutoff, >= 38°C emergency cool
    4. _checkHeaterRuntime — > 10 phút → cooldown 5 phút
    5. _checkMutualExclusion — heater+cooler, pH_up+pH_down, pump_in+pump_out
    6. _checkPhPumpTiming — interval tối thiểu 30 giây
    7. _checkShockGuard — shock flag → tạm dừng 1 chu kỳ

    Stale sensor mapping:
    - Temp stale → tắt heater + cooler
    - pH stale → tắt pH pumps
    - TDS stale → chỉ log (không có relay TDS)
    ※ LOẠI BỎ: Water stale → (không còn water sensor)

    Lưu ý: pump_in / pump_out được điều khiển bởi water_change_manager,
    safety_core CHỈ kiểm tra mutual exclusion (không bật cùng lúc),
    không can thiệp vào logic thời gian của water_change_manager.

────────────────────────────────────────────────
BƯỚC 7: CONTROLLERS
────────────────────────────────────────────────

Tạo 4 file:

7a. hysteresis_controller.h + .cpp
    State machine 3 trạng thái cho nhiệt độ:
    - IDLE → T < (target - deadband) → HEATING
    - IDLE → T > (target + deadband) → COOLING
    - HEATING → T >= (target + deadband) → IDLE
    - COOLING → T <= (target - deadband) → IDLE
    target = (temp_min + temp_max) / 2
    deadband = (temp_max - temp_min) / 4

    ※ LOẠI BỎ: Hysteresis mực nước (water filling/draining)
    ※ Pump In / Pump Out nay do water_change_manager điều khiển

7b. pid_controller.h + .cpp
    PID cho pH (giữ nguyên):
    - error = setpoint - measured
    - P = Kp × error
    - I += error × dt (có anti-windup, clamp ±INTEGRAL_MAX)
    - D = Kd × (error - prev_error) / dt
    - output = P + I + D (đơn vị ms, clamp ±3000ms)
    - Dead zone: |error| < 0.05 → output = 0
    - output > 0 → bật pH_UP (acid)
    - output < 0 → bật pH_DOWN (base)
    Safety Core kiểm tra interval 30s sau đó.

────────────────────────────────────────────────
BƯỚC 7.5: WATER CHANGE MANAGER — MODULE MỚI
────────────────────────────────────────────────

Tạo 2 file:

7.5a. water_change_manager.h

  struct WaterChangeConfig {
      bool     schedule_enabled  = false;
      uint8_t  schedule_hour     = 8;     // Giờ chạy (0-23)
      uint8_t  schedule_minute   = 0;     // Phút chạy (0-59)
      uint16_t pump_out_sec      = 30;    // Thời gian bơm ra (giây)
      uint16_t pump_in_sec       = 60;    // Thời gian bơm vào (giây)
  };

  class WaterChangeManager {
  public:
      void begin();
      void setConfig(const WaterChangeConfig& cfg);
      void update();                        // Gọi mỗi loop
      void triggerManual();                 // Kích hoạt từ Firebase hoặc nút bấm
      bool isBusy() const;                  // Đang thay nước?
      WaterChangeState getState() const;
      void getRelayCmd(bool& pump_out, bool& pump_in) const;
      String getStatusJson() const;         // Cho Firebase upload
  private:
      WaterChangeConfig _cfg;
      WaterChangeState  _state = IDLE;
      unsigned long     _stateStartMs = 0;
      uint32_t          _lastRunDay   = 0;  // epoch/86400
      bool              _manualTrigger = false;
      void _tick();
      bool _isScheduleTime() const;         // So sánh NTP
  };

7.5b. water_change_manager.cpp

  Luồng thay nước (state machine):
  ┌─────┐  triggerManual() hoặc lịch khớp  ┌─────────────┐
  │IDLE │ ─────────────────────────────────→│ PUMPING_OUT │
  └─────┘                                   └──────┬──────┘
                                                   │ sau pump_out_sec giây
                                            ┌──────▼──────┐
                                            │ PUMPING_IN  │
                                            └──────┬──────┘
                                                   │ sau pump_in_sec giây
                                            ┌──────▼──────┐
                                            │    DONE     │ → IDLE ngay sau 1 tick
                                            └─────────────┘

  _tick() logic:
  - State IDLE:
      Nếu NTP sync OK và schedule_enabled:
        Lấy giờ hiện tại (VN UTC+7)
        Nếu giờ:phút khớp schedule VÀ _lastRunDay != hôm nay:
          → chuyển PUMPING_OUT, _stateStartMs = millis()
      Nếu _manualTrigger == true:
        → chuyển PUMPING_OUT, _stateStartMs = millis()
        → _manualTrigger = false

  - State PUMPING_OUT:
      Bật relay pump_out
      Nếu millis() - _stateStartMs >= pump_out_sec × 1000:
        → tắt pump_out, chuyển PUMPING_IN, _stateStartMs = millis()

  - State PUMPING_IN:
      Bật relay pump_in
      Nếu millis() - _stateStartMs >= pump_in_sec × 1000:
        → tắt pump_in, chuyển DONE
        → _lastRunDay = ngày hôm nay (epoch/86400)

  - State DONE:
      → chuyển IDLE ngay lập tức
      → log "Water change complete"
      → upload Firebase: /water_change/last_run, /water_change/state

  getRelayCmd():
  - PUMPING_OUT → pump_out = true, pump_in = false
  - PUMPING_IN  → pump_in  = true, pump_out = false
  - IDLE/DONE   → pump_out = false, pump_in = false

  Nút bấm vật lý (PIN_BTN_WATER_CHANGE):
  - Được xử lý trong buttonManager hoặc main.cpp
  - Khi nhấn → gọi waterChangeManager.triggerManual()

  An toàn:
  - Nếu đang PUMPING_OUT mà nhấn nút lần nữa → IGNORE (đã bận)
  - Không chạy nếu NTP chưa sync và là lịch tự động (OK với manual)
  - Safety core vẫn kiểm tra mutual exclusion pump_in + pump_out

────────────────────────────────────────────────
BƯỚC 8: ANALYTICS
────────────────────────────────────────────────

Tạo 2 file:

8a. analytics.h
    - AnalyticsConfig: ema_alpha, cusum_k/threshold, wsi/fsi weights
    - EmaState, CusumState, DriftDir, AnalyticsResult (wsi, fsi)

8b. analytics.cpp
    Gọi mỗi 5s sau khi push CleanReading vào cleanBuffer:

    EMA: ema_new = α × value + (1-α) × ema_old
      α = 0.1, chỉ update khi source != FALLBACK_DEFAULT

    CUSUM: chỉ chạy trên MEASURED data
      S_up += (value - ema - k), clamp >= 0
      S_down += (ema - value - k), clamp >= 0
      Vượt threshold → báo DRIFT, reset S

    WSI = 100 - weighted sum of (std/mean) cho T, pH, TDS
    FSI = α×|ΔT| + β×|ΔpH| + penalty × shocks

────────────────────────────────────────────────
BƯỚC 9: OLED DISPLAY + BUTTON MANAGER
────────────────────────────────────────────────

Tạo 4 file:

9a. oled_display.h + .cpp
    SSD1306 128x64 SPI, thư viện Adafruit
    4 trang:
    - PAGE_SENSORS: T, pH, TDS, Water + source indicators
    - PAGE_ANALYTICS: EMA, WSI bar, FSI bar, Drift status
    - PAGE_RELAY: 6 relay ON/OFF + indicator dots
    - PAGE_SYSTEM: State, uptime, heap, WiFi
    Render throttle 500ms

9b. button_manager.h + .cpp
    6 nút (INPUT_PULLUP), debounce 50ms
    Danh sách nút:
    - BTN_PAGE          → GPIO 25
    - BTN_UP            → GPIO 33
    - BTN_DOWN          → GPIO 32
    - BTN_SELECT        → GPIO 22  ← ĐÃ ĐỔI (tránh trùng OLED DC GPIO 15)
    - BTN_BACK          → GPIO 0
    - BTN_WATER_CHANGE  → GPIO 2   ← NÚT MỚI (kích hoạt thay nước thủ công)
    wasPressed(id) → true 1 lần sau khi nhấn

────────────────────────────────────────────────
BƯỚC 10: WIFI + FIREBASE  ← ĐÃ TRIỂN KHAI (bao gồm cả Bước 17 cũ)
────────────────────────────────────────────────

Tạo 2 file:

10a. wifi_firebase.h + .cpp
    WiFiManager: non-blocking connect, auto-reconnect mỗi 10s

    FirebaseClient: dùng thư viện FirebaseESP32 (mobizt)
      https://github.com/mobizt/Firebase-ESP32
      PlatformIO: lib_deps = mobizt/Firebase ESP32 Client@^4.4.17

    ── KIẾN TRÚC: 1 UNIFIED STREAM (thay vì nhiều stream riêng) ────
    FirebaseData objects: 2 (fbStream + fbUpload) thay vì 5
      - fbStream:  BSSL rx=2048, tx=512  (stream nhận JSON node cha)
      - fbUpload:  BSSL rx=4096, tx=1024 (upload/push data)
      → Tiết kiệm ~20-25KB heap SSL so với kiến trúc cũ
      → 1 TLS session duy nhất — không bao giờ reconnect đồng thời

    Heap guard: MIN_FREE_HEAP_BYTES = 20000 (20KB)
      → Kiểm tra ESP.getFreeHeap() trước mọi thao tác Firebase
      → Nếu heap thấp → bỏ qua cycle, hệ thống điều khiển không bị ảnh hưởng

    UPLOAD (ESP32 → Firebase, PUT mỗi 5 giây):
      /telemetry/     — sensor data + source + status + shock
      /analytics/     — ema, wsi, fsi, drift
      /relay_state/   — 6 relay ON/OFF
      /status/        — online, uptime, heap, wifi_rssi
      /water_change/state + last_run

    STREAM (Firebase → ESP32, real-time):
      1 stream duy nhất tại /devices/{ID}/ (node cha).
      Firebase RTDB tự động gửi event cho mọi node con thay đổi,
      kèm data.dataPath() để phân biệt nguồn.

      Callback onUnifiedStream(data):
        data.dataPath() → xác định nhánh:

        "/config"           → parse → validate → apply ControlConfig
                              → hysteresisCtrl.setConfig() + pidCtrl.setConfig()

        "/pipeline_config"  → parse → validate → apply PipelineConfig
                              → dataPipeline.setConfig()

        "/safety_limits"    → parse → validate → apply SafetyLimits
                              → safetyCore.setLimits()

        "/water_change"     → parse WaterChangeConfig + detect trigger
                              → nếu trigger==true: waterChangeManager.triggerManual()
                              → reset /water_change/trigger = false ngay lập tức
                              → waterChangeManager.setConfig()

        "/"  (root event)   → Firebase gửi toàn bộ JSON khi lần đầu connect
                              → gọi tuần tự tất cả parser với cùng JSON string

        Lọc bỏ: /telemetry, /analytics, /relay_state, /status, /safety_events
                (dữ liệu do chính thiết bị ghi — tránh callback spam)

      Stream health check: mỗi 60s (STREAM_RECONNECT_INTERVAL_MS)
        → kiểm tra fbStream.streamAvailable() + httpCode
        → nếu mất → _beginStream() reconnect 1 lần duy nhất

    Config apply flow (trong callback):
      1. Parse JSON từ streamData
      2. Validate (range check, cross-validate)
      3. Nếu OK → apply ngay, ghi log INFO
      4. Nếu FAIL → giữ config cũ, ghi log WARNING
      5. Firmware luôn có default compile-time làm fallback

    Safety events PUSH (khi có event mới):
      /safety_events/ — pushJSON với timestamp
      Chỉ push khi event != NONE và event != lastPushedEvent
      (tránh push trùng lặp liên tục)

    Database path: /devices/{DEVICE_ID}/...

────────────────────────────────────────────────
BƯỚC 11: SYSTEM MANAGER + MAIN
────────────────────────────────────────────────

Tạo 4 file:

11a. system_manager.h + .cpp
    - begin(): init theo thứ tự cố định
      Logger → SafetyCore → Config → Sensor → Controllers →
      WaterChangeManager → Analytics → Watchdog(30s) → WiFi → NTP sync
    - update(): feed watchdog, check safe mode conditions
    - Safe mode triggers:
        Mất temp > 60s
        FSI > 50 liên tục 5 chu kỳ
    - Safe mode: emergencyOff(), vẫn đọc sensor + gửi telemetry
    - NTP sync: configTime(NTP_GMT_OFFSET_SEC, 0, NTP_SERVER)
      Gọi sau khi WiFi kết nối. Water change manager chờ NTP sync
      trước khi chạy lịch tự động.

11b. main.cpp
    setup(): systemManager.begin(), buttonManager.begin(), oledDisplay.begin()
    loop(): 18 bước như mô tả ở mục MAIN LOOP

    pH pulse timer: non-blocking
    - startPhPulse(relay, ms): ghi thời điểm tắt
    - tickPhPulse(): tắt relay khi hết thời gian

    Water change button handler:
    - if buttonManager.wasPressed(BTN_WATER_CHANGE):
        waterChangeManager.triggerManual()
    - Firebase manual trigger:
        Stream /water_change/trigger → nếu true → triggerManual(), reset về false

────────────────────────────────────────────────
BƯỚC 12: UNIT TESTS + STRESS TESTS
────────────────────────────────────────────────

Tạo 3 file:

12a. test_pipeline.h — header
12b. testpipeline.cpp — unit tests
12c. test_random_stress.h — stress tests

Unit test suites:
  Suite 1: Range Gate
    - T in range → MEASURED
    - T out of range → FALLBACK_LAST với đúng last_good
    - NaN → FALLBACK
    - Boot + NaN → FALLBACK_DEFAULT = 25.0

  Suite 2: MAD Filter (verify đọc validRawBuffer)
    - Buffer < 10 → bypass MAD
    - Spike 1 mẫu → reject, dùng median(validRaw)
    - Trend tăng dần → accept (KHÔNG BỊ KHOÁ)  ← test quan trọng nhất
    - Spike rồi về → reject spike

  Suite 3: Shock Detection
    - Delta lớn → shock flag true, VẪN accept data
    - Delta nhỏ → no shock

  Suite 4: Fallback Tiers
    - DEFAULT khi boot
    - LAST khi có prior reading
    - MEDIAN khi MAD reject + đủ buffer

  Suite 5: Stale Sensor (tích hợp Safety Core)
    - 6 chu kỳ fallback → Safety tắt relay
    - Sensor recover → relay hoạt động lại
    - pH stale, temp OK → chỉ tắt pH pump

  Suite 6: Regression
    - Trend chậm 31→34 qua 20 chu kỳ → MỌI giá trị MEASURED
    - Sensor hư + fallback pH=8 → Safety cắt bơm sau 6 chu kỳ
    - 100 chu kỳ bình thường → >= 98 MEASURED

Stress test suites:
  Suite A: 100 normal samples (noise ±0.2°C, ±0.05pH)
  Suite B: 15 random spikes
  Suite C: Sensor dead 20 cycles → recover
  Suite D: Trend tăng 0.15°C × 30 cycles (27→31.5)
  Suite E: Mixed (70% OK, 20% range fail, 10% spike)

Chạy test: uncomment #define RUN_PIPELINE_TESTS trong main.cpp


================================================================
FIREBASE DATABASE STRUCTURE (mở rộng cho Web Dashboard)
================================================================

Luồng dữ liệu real-time (KHÔNG polling):

  ┌──────────┐   stream (SSE)    ┌──────────────────┐  onValue()   ┌───────────┐
  │  ESP32   │ ←──────────────── │  Firebase RTDB   │ ──────────→ │ Dashboard │
  │          │ ──────────────→── │                  │ ←────────── │           │
  └──────────┘   PUT mỗi 30s    └──────────────────┘  set/update  └───────────┘

  Dashboard GHI config → Firebase cập nhật → ESP32 nhận NGAY qua stream
  ESP32 GHI telemetry  → Firebase cập nhật → Dashboard nhận NGAY qua onValue

/devices/{DEVICE_ID}/
│
├── telemetry/                  ← ESP32 GHI mỗi 5s, Dashboard ĐỌC
│   ├── temperature: 27.3
│   ├── ph: 7.1
│   ├── tds: 450
│   ├── source_temp: "MEASURED"
│   ├── source_ph: "MEASURED"
│   ├── source_tds: "MEASURED"
│   ├── status_temp: "OK"
│   ├── status_ph: "OK"
│   ├── status_tds: "OK"
│   ├── shock_temperature: false
│   ├── shock_ph: false
│   └── timestamp: 1709827200000
│   ※ LOẠI BỎ: water_level_cm, source_water, status_water
│
├── analytics/                  ← ESP32 GHI, Dashboard ĐỌC
│   ├── ema_temp: 27.2
│   ├── ema_ph: 7.05
│   ├── ema_tds: 448
│   ├── wsi: 85.3
│   ├── fsi: 12.1
│   ├── drift_temp: "NONE"
│   ├── drift_ph: "NONE"
│   └── drift_tds: "NONE"
│
├── relay_state/                ← ESP32 GHI, Dashboard ĐỌC
│   ├── heater: false
│   ├── cooler: false
│   ├── ph_up: false
│   ├── ph_down: false
│   ├── pump_in: false
│   └── pump_out: false
│
├── water_change/               ← 2 chiều: ESP32 GHI trạng thái, Dashboard GHI lệnh
│   ├── state: "IDLE"           ← ESP32 GHI: "IDLE"|"PUMPING_OUT"|"PUMPING_IN"
│   ├── last_run: 1709827200000 ← ESP32 GHI: timestamp lần chạy gần nhất
│   ├── trigger: false          ← Dashboard GHI: true → ESP32 gọi triggerManual()
│   └── schedule/               ← Dashboard USER GHI, ESP32 NHẬN qua stream
│       ├── enabled: false
│       ├── hour: 8
│       ├── minute: 0
│       ├── pump_out_sec: 30
│       └── pump_in_sec: 60
│
├── status/                     ← ESP32 GHI, Dashboard ĐỌC
│   ├── online: true
│   ├── uptime_sec: 3600
│   ├── free_heap: 120000
│   ├── wifi_rssi: -45
│   ├── safe_mode: false
│   ├── last_seen: 1709827200000
│   └── firmware_version: "2.0.0"
│
├── safety_events/              ← ESP32 GHI, Dashboard ĐỌC
│   └── {push_id}/
│       ├── event: "THERMAL_CUTOFF"
│       ├── value: 42.5
│       ├── action: "ALL_OFF"
│       └── timestamp: 1709827200000
│
├── config/                     ← Dashboard USER GHI, ESP32 NHẬN real-time qua stream
│   ├── temp_min: 25.0
│   ├── temp_max: 30.0
│   ├── ph_min: 6.5
│   ├── ph_max: 7.5
│   ├── tds_target: 400
│   ├── tds_tolerance: 100
│   ├── pid_kp: 200.0
│   ├── pid_ki: 50.0
│   ├── pid_kd: 30.0
│   └── updated_at: 1709827200000
│   ※ LOẠI BỎ: water_min_cm, water_max_cm (không còn điều khiển theo mực nước)
│   ※ Lịch thay nước → /water_change/schedule/
│
├── pipeline_config/            ← Dashboard ADMIN GHI, ESP32 NHẬN real-time qua stream
│   ├── temp_range_min: 15.0
│   ├── temp_range_max: 40.0
│   ├── ph_range_min: 4.0
│   ├── ph_range_max: 10.0
│   ├── tds_range_min: 1.0
│   ├── tds_range_max: 3000.0
│   ├── mad_window_size: 30
│   ├── mad_min_samples: 10
│   ├── mad_threshold: 3.5
│   ├── mad_floor_temp: 0.30
│   ├── mad_floor_ph: 0.08
│   ├── mad_floor_tds: 3.0
│   ├── shock_temp_delta: 3.0
│   ├── shock_ph_delta: 0.5
│   └── updated_at: 1709827200000
│   ※ LOẠI BỎ: water_range_min, water_range_max, mad_floor_water
│
├── safety_limits/              ← Dashboard ADMIN GHI, ESP32 NHẬN real-time qua stream
│   ├── thermal_cutoff_c: 42.0
│   ├── temp_emergency_cool_c: 38.0
│   ├── heater_max_runtime_min: 10
│   ├── heater_cooldown_min: 5
│   ├── ph_pump_max_pulse_ms: 3000
│   ├── ph_pump_min_interval_ms: 30000
│   ├── stale_sensor_threshold: 6
│   └── updated_at: 1709827200000
│   ※ LOẠI BỎ: water_critical_low_cm (không còn cảm biến mực nước)
│
└── history/                    ← ESP32 GHI mỗi 5 phút (optional)
    └── {YYYY-MM-DD}/
        └── {push_id}/
            ├── t: 27.3
            ├── p: 7.1
            ├── d: 450
            ├── w: 35.2
            ├── wsi: 85
            └── ts: 1709827200000


================================================================
WEB DASHBOARD — THIẾT KẾ TỔNG QUAN
================================================================

Công nghệ: HTML + CSS + JavaScript thuần (không framework)
Giao tiếp: Firebase Realtime Database SDK (firebase-app, firebase-database)
Hosting: Firebase Hosting hoặc mở local

Cấu trúc thư mục:
  web/
  ├── index.html              ← Trang chính: xem real-time data
  ├── user-settings.html      ← User: chỉnh thông số vận hành
  ├── admin.html              ← Admin: chỉnh thông số pipeline + safety
  ├── css/
  │   └── style.css           ← CSS chung cho cả 3 trang
  ├── js/
  │   ├── firebase-init.js    ← Khởi tạo Firebase app
  │   ├── dashboard.js        ← Logic trang index
  │   ├── user-settings.js    ← Logic trang user settings
  │   └── admin.js            ← Logic trang admin
  └── assets/
      └── fish-icon.svg       ← Icon (optional)

Nguyên tắc thiết kế (v6.0 — phong cách Blynk-inspired, đơn giản):
  1. Real-time: dùng onValue() listener, data tự cập nhật
  2. Validation phía client trước khi ghi Firebase
  3. Confirm dialog trước khi lưu thay đổi
  4. Visual feedback tối giản: chỉ dùng màu khi cần thiết (xanh OK, đỏ danger)
  5. Responsive: dùng được trên mobile

  TRIẾT LÝ GIAO DIỆN (Blynk-inspired):
  - Nền tối (#1a1a2e hoặc #0f1117) — dễ đọc trong môi trường ánh sáng yếu bên bể cá
  - Card nền tối hơn (#16213e / #1e2235) — border radius lớn (12-16px), shadow nhẹ
  - Font: monospace/system-ui cho số đo, sans-serif cho label
  - Màu accent: mỗi sensor 1 màu cố định (nhiệt độ = cam, pH = xanh lá, TDS = tím)
  - KHÔNG dùng: gradient phức tạp, animation nhiều bước, shadow đổ bóng nhiều lớp
  - Gauge tròn: SVG arc đơn giản, chỉ vẽ 1 đường cong màu trên nền xám
  - Toggle switch: CSS thuần, không dùng thư viện ngoài
  - Số liệu: font-size lớn, dễ đọc từ xa (≥ 2rem cho giá trị chính)
  - Padding thoáng, ít element trên 1 card — mỗi card 1 thông tin chính


────────────────────────────────────────────────
BƯỚC 13: WEB — FIREBASE INIT + CSS CHUNG
────────────────────────────────────────────────

Tạo 2 file:

13a. js/firebase-init.js
    - Import Firebase SDK (CDN, dùng module hoặc compat)
    - firebaseConfig object (apiKey, databaseURL, ...)
    - Export: database reference, DEVICE_ID constant
    - Helper: getRef(path) → ref(db, `/devices/${DEVICE_ID}/${path}`)
    - Helper: updateTimestamp(refPath) → set updated_at

13b. css/style.css  — PHONG CÁCH BLYNK-INSPIRED (v6.0)

    PALETTE MÀU:
      --bg-page:    #0f1117   (nền trang)
      --bg-card:    #1a1d2e   (nền card)
      --bg-card2:   #1e2235   (nền card phụ / panel trong card)
      --border:     #2a2d3e   (viền card)
      --text-main:  #e8e8f0   (chữ chính)
      --text-sub:   #7a7d8e   (chữ phụ, label)
      --accent-temp: #f5a623  (cam — nhiệt độ)
      --accent-ph:   #4ecdc4  (xanh ngọc — pH)
      --accent-tds:  #a78bfa  (tím nhạt — TDS)
      --accent-ok:   #2dd4bf  (xanh — trạng thái tốt)
      --accent-warn: #fbbf24  (vàng — cảnh báo)
      --accent-err:  #f87171  (đỏ — lỗi/nguy hiểm)
      --toggle-on:   #2dd4bf  (toggle bật)
      --toggle-off:  #3a3d4e  (toggle tắt)

    COMPONENTS:
      .card
        background: var(--bg-card)
        border: 1px solid var(--border)
        border-radius: 14px
        padding: 20px
        (KHÔNG shadow nhiều lớp — chỉ 1 shadow nhẹ nếu cần)

      .gauge-container
        Vẽ bằng SVG: 1 circle nền xám + 1 arc màu theo sensor
        Giá trị hiển thị ở giữa: font-size 1.8rem, font-weight 600
        Label nhỏ bên dưới: font-size 0.75rem, color var(--text-sub)
        Không có animation quay — chỉ update strokeDashoffset tức thì

      .toggle-switch
        CSS thuần, không thư viện
        Track: 44px × 24px, border-radius 12px
        Thumb: 18px × 18px, dịch chuyển 20px khi bật
        Transition: 0.2s ease (ngắn, không lòe loẹt)
        Chỉ READ-ONLY trên dashboard (hiển thị trạng thái relay thực tế)

      .relay-card
        Nhỏ gọn: icon (24px) + tên + toggle trong 1 hàng
        Background thay đổi nhẹ khi ON (tăng độ sáng card)
        Không badge màu phức tạp

      .status-dot
        8px circle, màu theo trạng thái
        Chỉ blink khi ERROR — trạng thái bình thường không animation

      .value-label  (số đo lớn)
        font-size: 2rem, font-weight: 700
        font-family: 'JetBrains Mono', monospace (fallback: monospace)

      .section-title
        font-size: 0.7rem, letter-spacing: 0.1em, text-transform: uppercase
        color: var(--text-sub)

      Button:
        .btn-primary: background var(--accent-ok), không gradient
        .btn-danger:  background var(--accent-err)
        .btn-ghost:   border 1px solid var(--border), transparent bg
        border-radius: 8px, padding: 8px 16px
        KHÔNG hover animation phức tạp — chỉ opacity 0.85

      Form input (settings pages):
        background: var(--bg-card2), border: 1px solid var(--border)
        border-radius: 8px, color: var(--text-main)
        focus: border-color var(--accent-ok), không glow lớn

      Responsive (mobile-first):
        Grid sensor cards: 1 col (mobile) → 3 col (≥ 768px)
        Grid relay: 2 col (mobile) → 3 col (≥ 480px)
        Nav: collapse thành hamburger ở mobile


────────────────────────────────────────────────
BƯỚC 14: WEB — TRANG DASHBOARD (index.html)  ← CẬP NHẬT v6.0
────────────────────────────────────────────────

Tạo 2 file:

14a. index.html
    Layout đơn giản, nền tối, đọc được từ xa — style Blynk.

    PHẦN 1 — HEADER (thanh ngang gọn):
      - Tên "🐟 Aquarium" (trái)
      - Status badge nhỏ: chấm xanh + "Online" / chấm đỏ + "Offline"
      - Uptime: "↑ 2h 35m" (chữ nhỏ, màu text-sub)
      - Nav links: Dashboard | Settings | Admin (phải)
      - Safe Mode: banner đỏ nhỏ xuất hiện phía dưới header nếu active
      KHÔNG có: subtitle, breadcrumb, shadow header phức tạp

    PHẦN 2 — SENSOR CARDS (grid 3 cột, mỗi card 1 gauge SVG):

      Mỗi card gồm:
        ┌─────────────────────────────┐
        │  NHIỆT ĐỘ          • MEAS  │  ← section-title + status dot
        │                             │
        │       ╭─────╮               │
        │      ╱  27.3 ╲              │  ← gauge SVG arc (màu cam)
        │     │    °C   │             │     arc từ 180° đến 0° (nửa vòng tròn)
        │      ╲       ╱              │     range: 15–40°C
        │       ╰─────╯               │
        │                             │
        │  15°C ━━━━━━━━━━━━ 40°C    │  ← min/max nhỏ bên dưới
        └─────────────────────────────┘

      Card Nhiệt độ:
        - Arc màu: var(--accent-temp) = cam
        - Range gauge: temp_range_min → temp_range_max (từ pipeline_config)
        - Số giữa: "27.3" + "°C" bên dưới
        - Dot màu: xanh (OK) / cam (MAD_OUTLIER) / đỏ (OUT_OF_RANGE/SENSOR_ERROR)
        - Badge nhỏ góc phải: "MEAS" (xanh tím nhạt) hoặc "FALLBACK" (vàng)
        - Shock: đổi border card thành --accent-warn trong 3 giây rồi về bình thường

      Card pH:
        - Arc màu: var(--accent-ph) = xanh ngọc
        - Range: 4–10
        - Số giữa: "7.10"
        - Không có thanh pH gradient riêng — gauge SVG đã đủ thông tin

      Card TDS:
        - Arc màu: var(--accent-tds) = tím
        - Range: 0–1000 ppm (hoặc tds_range_max)
        - Số giữa: "450" + "ppm"
        - Nếu vượt target±tolerance: dot vàng

      ※ LOẠI BỎ so với v5: EMA trend arrow, thanh pH gradient, con trỏ pH bar

    PHẦN 3 — RELAY PANEL (grid 2×3, compact):

      Tiêu đề section: "RELAY" (chữ nhỏ uppercase)

      Mỗi relay card gồm 1 hàng ngang:
        [icon]  Heater          [toggle OFF/ON]

      6 relay theo thứ tự: Heater | Cooler | pH Up | pH Down | Pump In | Pump Out
      Icon đơn giản (emoji hoặc SVG 1 màu):
        🔥 Heater  ❄️ Cooler  ⬆️ pH Up  ⬇️ pH Down  💧 Pump In  🚰 Pump Out

      Toggle: READ-ONLY — hiển thị trạng thái thực tế từ Firebase relay_state
        ON  → toggle sáng màu var(--accent-ok) + nền card tăng nhẹ
        OFF → toggle xám var(--toggle-off)

      KHÔNG có: manual override toggle, icon phức tạp

    PHẦN 4 — WATER CHANGE PANEL (1 card ngang gọn):

      ┌──────────────────────────────────────────────────────┐
      │ 💧 THAY NƯỚC    [IDLE ●]    Lần cuối: hôm nay 08:03 │
      │                                                      │
      │ [████████████░░░░░░░░] 45%  "Đang bơm ra..."        │  (chỉ hiện khi đang chạy)
      │                                                      │
      │ Lịch: Tự động 08:00 mỗi ngày    [Thay nước ngay →]  │
      └──────────────────────────────────────────────────────┘

      - State badge: "IDLE" (xám) / "BƠM RA" (cam) / "BƠM VÀO" (xanh)
      - Progress bar: chỉ hiện khi state != IDLE, tính % từ millis
      - Nút "Thay nước ngay": .btn-primary, disable + xám khi đang bận
      - Lịch info: 1 dòng text nhỏ
      KHÔNG có: animation phức tạp, nhiều row thông tin

    PHẦN 5 — ANALYTICS PANEL (2 gauge nhỏ + drift text):

      Layout 2 cột:
        Cột trái — WSI gauge (arc nhỏ, 80px):
          "WSI" label + số + màu theo ngưỡng
          > 70: xanh | 40–70: vàng | < 40: đỏ
        Cột phải — FSI gauge (tương tự, ngược chiều):
          "FSI" label + số
          < 30: xanh | 30–60: vàng | > 60: đỏ

        Dưới 2 gauge — Drift 1 dòng:
          "Drift: T→ NONE  pH→ UP  TDS→ NONE"
          UP/DOWN đổi màu theo chiều, NONE giữ text-sub

      KHÔNG có: gauge vòng tròn full-size, chart lịch sử

    PHẦN 6 — SAFETY EVENT LOG (bảng nhỏ gọn):

      Tiêu đề: "SAFETY LOG" + badge đếm số event hôm nay
      Bảng tối giản (không border table):
        Mỗi dòng: [dot màu] [HH:MM] [event name] [giá trị]
        Tối đa 10 dòng hiển thị, scroll nếu nhiều hơn
        Màu dot: đỏ (THERMAL_CUTOFF, EMERGENCY_COOL) / cam (khác)
        Không có cột "action" — quá dài, bỏ đi

      KHÔNG có: badge riêng cho severity, nhiều cột

14b. js/dashboard.js
    - onValue listeners: telemetry, analytics, relay_state, status,
                          safety_events, water_change
    - updateGauge(id, value, min, max, color)  ← SVG arc helper
    - updateRelayToggle(name, state)
    - updateWaterChangePanel(wcData)
    - updateAnalytics(aData)
    - appendSafetyLog(events)
    - .info/connected → show/hide offline banner
    - Timestamp formatter: giờ VN (UTC+7), format "HH:MM"
    - Uptime formatter: "Xh Ym"


────────────────────────────────────────────────
BƯỚC 15: WEB — TRANG USER SETTINGS  ← CẬP NHẬT v6.0 (style)
────────────────────────────────────────────────

Tạo 2 file:

15a. user-settings.html
    Chỉnh các thông số trong /config/ và /water_change/schedule/
    Style: nền tối, card-based, input gọn — nhất quán với dashboard

    LAYOUT TỔNG QUAN:
      Header: giống dashboard (nav + status)
      Body: 1 cột trung tâm (max-width: 600px), các section là card nối tiếp nhau
      Footer: sticky bottom trên mobile với nút Save/Cancel

    SECTION 1 — NHIỆT ĐỘ:
      Card với label "NHIỆT ĐỘ" (uppercase nhỏ, màu accent-temp)
      - 2 input cạnh nhau: "Min °C" / "Max °C"
      - Dưới input: preview text nhỏ "→ Mục tiêu 27.5°C ± 1.25°C"
      - Validation inline: border đỏ + text lỗi nhỏ nếu sai
      KHÔNG có: dual-thumb range slider (phức tạp, khó mobile)

    SECTION 2 — pH:
      Card với label "ĐỘ pH" (màu accent-ph)
      - 2 input: "pH Min" / "pH Max"
      - Preview: "→ Mục tiêu 7.00 ± 0.50"
      KHÔNG có: thanh pH gradient visual

    SECTION 3 — TDS:
      Card với label "TDS" (màu accent-tds)
      - 2 input: "Mục tiêu (ppm)" / "Dung sai (ppm)"
      - Preview: "→ 400 ± 100 ppm"

    SECTION 4 — THAY NƯỚC TỰ ĐỘNG:
      Card với label "THAY NƯỚC"
      - Toggle "Bật lịch tự động" (toggle switch CSS)
        → khi tắt: fade out các input bên dưới (opacity 0.3, pointer-events none)
      - 2 input hàng ngang: "Giờ" (select 0-23) + "Phút" (select 0/15/30/45)
      - Preview: "→ Tự động 08:00 mỗi ngày"
      - 2 input: "Bơm ra (giây)" / "Bơm vào (giây)"
      - Nút "💧 Thay nước ngay" (btn-primary, full-width)
        + text nhỏ "Lần cuối: hôm nay 08:03" bên dưới

    SECTION 5 — PID pH (ẩn mặc định):
      Card collapsible: click "▸ Cài đặt nâng cao PID" để mở
      - 3 input: Kp / Ki / Kd

    FOOTER STICKY:
      - Nút "Lưu" (btn-primary) + Nút "Hủy" (btn-ghost)
      - Hiển thị "● 3 thay đổi chưa lưu" nếu isDirty()
      - Toast notification nhỏ (bottom-right, tự ẩn sau 3s)

15b. js/user-settings.js  (logic giữ nguyên như v5, không đổi)


────────────────────────────────────────────────
BƯỚC 16: WEB — TRANG ADMIN  ← CẬP NHẬT v6.0 (style)
────────────────────────────────────────────────

Tạo 2 file:

16a. admin.html
    Style: nhất quán với dashboard và user-settings (nền tối, card)
    Khác biệt duy nhất với user-settings: có banner cảnh báo + section Safety viền đỏ

    BANNER ĐẦU TRANG:
      1 dòng ngang: ⚠️ "Trang Admin — thay đổi ảnh hưởng trực tiếp đến an toàn hệ thống"
      Background: rgba(248, 113, 113, 0.15), border-left: 3px solid var(--accent-err)
      KHÔNG phải modal popup, KHÔNG chặn trang

    SECTION 1 — RANGE GATE:
      Card label "RANGE GATE" — layout bảng 3 dòng:
        Cột: Sensor | Min | Max
        Mỗi dòng là 1 sensor với badge màu (cam/ngọc/tím)
        Input nhỏ gọn (width: 80px)

    SECTION 2 — MAD FILTER:
      Card label "MAD FILTER"
      - mad_window_size, mad_min_samples, mad_threshold: 3 input hàng ngang
      - mad_threshold: thêm input[type=range] ngay bên dưới (đơn giản, 1 thumb)
        label "Nhạy" bên trái, "Thô" bên phải — không cầu kỳ
      - MAD Floor: bảng 3 input nhỏ (T | pH | TDS) trong 1 hàng

    SECTION 3 — SHOCK DETECTION:
      Card label "SHOCK"
      - 2 input hàng ngang: ΔT / ΔpH

    SECTION 4 — SAFETY LIMITS:
      Card label "⚠️ SAFETY LIMITS"
      Border: 1px solid var(--accent-err) (thay vì nền đỏ phức tạp)
      - Layout: 2 cột input, mỗi input có label ngắn phía trên
      - Nhóm liên quan gần nhau:
          [thermal_cutoff] [temp_emergency_cool]
          [heater_max_runtime] [heater_cooldown]
          [ph_pump_max_pulse] [ph_pump_min_interval]
          [stale_sensor_threshold]

    SECTION 5 — ANALYTICS CONFIG (optional):
      Card collapsible "▸ Analytics (nâng cao)"
      - ema_alpha, cusum_k, cusum_threshold

    FOOTER (sticky bottom, nhất quán user-settings):
      - "● N thay đổi chưa lưu"
      - Nút "Lưu" → confirm dialog yêu cầu gõ "CONFIRM"
      - Nút "Export JSON" (btn-ghost)
      - Nút "Import JSON" (btn-ghost)
      - Nút "Reset mặc định" (btn-danger, tách xa các nút khác)

16b. js/admin.js  (logic giữ nguyên như v5, không đổi)
    - Validation nghiêm ngặt hơn:
        thermal_cutoff > temp_emergency_cool
        ph_pump_min_interval > ph_pump_max_pulse × 2
        Không cho phép range gate hẹp hơn user config range
        Cross-validate: safety range phải bao user range
    - Confirm dialog đặc biệt: phải gõ "CONFIRM" thay vì chỉ click OK
    - Change diff display trước khi lưu
    - Export/Import JSON config
    - Logging: ghi lại ai thay đổi gì lúc nào (optional)


────────────────────────────────────────────────
BƯỚC 17: ĐÃ TÍCH HỢP VÀO BƯỚC 10
────────────────────────────────────────────────

Bước 17 trong blueprint v4 (mở rộng stream từ 1→4 stream) đã được
thực hiện trực tiếp trong Bước 10 với kiến trúc tốt hơn:

  THAY ĐỔI SO VỚI KẾ HOẠCH BAN ĐẦU:
  - Kế hoạch cũ: Bước 10 dùng 3 stream, Bước 17 mở rộng lên 4 stream
    → 4 stream × BSSL rx=8192 ≈ 40KB heap SSL thường trực
    → 4 SSL reconnect đồng thời → burst OOM → StoreProhibited crash

  - Thực tế (v5.0): 1 unified stream tại node cha từ Bước 10
    → 2 objects × BSSL → ~8-10KB heap SSL
    → 1 TLS session, không OOM
    → Trigger water_change vẫn realtime như thiết kế

  Tất cả nội dung của Bước 17 (ControlConfig, PipelineConfig,
  SafetyLimits, WaterChangeConfig, safety_events, relay_state,
  water_change upload) đều đã có trong wifi_firebase.h/cpp hiện tại.

  ※ KHÔNG CẦN THỰC HIỆN BƯỚC 17 RIÊNG.
     Chỉ cần Bước 10 là đủ.

17b. safety_core.h + .cpp — vẫn cần cập nhật riêng:
    Thay đổi (nếu chưa làm ở Bước 6):
    - HardLimits không còn là namespace constant
    - Chuyển thành struct SafetyLimits, có giá trị default
    - safetyCore.setLimits(SafetyLimits) → cập nhật runtime
    - safetyCore.getLimits() → trả về limits hiện tại (dùng trong stream callback)
    - SafetyLimits validator: không cho phép giá trị nguy hiểm
      (ví dụ: thermal_cutoff > 50°C → reject)

Lưu ý: firmware vẫn có giá trị mặc định compile-time.
Nếu Firebase không có config → dùng default. Không bao giờ chạy
mà không có config hợp lệ.


================================================================
CẤU HÌNH CROSS-VALIDATION GIỮA CÁC TẦNG CONFIG
================================================================

Khi user hoặc admin thay đổi config, cần đảm bảo tính nhất quán:

  ADMIN RANGE (pipeline_config)    ⊇    USER RANGE (config)
  ────────────────────────────────────────────────────
  temp_range: [15, 40]              ⊇    temp: [25, 30]
  ph_range: [4, 10]                 ⊇    ph: [6.5, 7.5]
  ※ LOẠI BỎ: water range (không còn cảm biến mực nước)

  SAFETY LIMITS (safety_limits)     >    USER RANGE (config)
  ────────────────────────────────────────────────────
  thermal_cutoff: 42                >    temp_max: 30
  temp_emergency_cool: 38           >    temp_max: 30
  ※ LOẠI BỎ: water_critical_low check

Validation rule:
  - Trang User: không cho phép nhập ngoài admin range
    → input min/max bị giới hạn bởi pipeline_config
  - Trang Admin: cảnh báo nếu thu hẹp range mà user config
    hiện tại nằm ngoài range mới
  - ESP32: validate lần cuối, reject nếu bất hợp lệ


================================================================
TỔNG KẾT FILE LIST (28 files firmware + 9 files web = 37 files)
================================================================

FIRMWARE (28 files):

  Header-only (4):
    system_constants.h, type_definitions.h,
    circular_buffer.h, credentials.h

  Modules (12 cặp = 24 files):
    logger.h/cpp
    sensor_manager.h/cpp        ← Bỏ HC-SR04
    data_pipeline.h/cpp         ← CORE MỚI (bỏ water field)
    control_config.h/cpp        ← Bỏ water_min/max
    config_manager.h/cpp
    safety_core.h/cpp           ← CẬP NHẬT: bỏ water level check, thêm setLimits()/getLimits()
    hysteresis_controller.h/cpp ← Bỏ water hysteresis
    pid_controller.h/cpp
    water_change_manager.h/cpp  ← MODULE MỚI (bước 7.5)
    analytics.h/cpp
    oled_display.h/cpp
    wifi_firebase.h/cpp         ← ĐÃ TRIỂN KHAI v5.0: unified stream, 2 BSSL objects,
                                   heap guard, upload 5s, tích hợp cả Bước 17 cũ
    button_manager.h/cpp        ← 6 nút, SELECT → GPIO 22
    system_manager.h/cpp        ← Thêm NTP init
    main.cpp

  Tests (3):
    test_pipeline.h
    testpipeline.cpp
    test_random_stress.h

  Thư viện cần cài (PlatformIO/Arduino):
    - OneWire
    - DallasTemperature
    - Adafruit SSD1306
    - Adafruit GFX Library
    - mobizt/Firebase ESP32 Client@^4.4.17  ← FirebaseESP32 (#include <FirebaseESP32.h>)

WEB DASHBOARD (9 files):

  HTML (3):
    index.html                   ← Dashboard real-time
    user-settings.html           ← User config
    admin.html                   ← Admin config

  JavaScript (4):
    js/firebase-init.js          ← Firebase khởi tạo
    js/dashboard.js              ← Logic trang chính
    js/user-settings.js          ← Logic trang user
    js/admin.js                  ← Logic trang admin

  CSS (1):
    css/style.css                ← Theme + components

  Assets (1):
    assets/fish-icon.svg         ← Icon (optional)

  CDN Dependencies (web):
    - Firebase JS SDK v9+ (modular, từ CDN)
      firebase-app, firebase-database
    - onValue(), set(), update(), push(), ref()
      → real-time listener, tự cập nhật khi data thay đổi


================================================================
GHI CHÚ CÁCH SỬ DỤNG BẢN KẾ HOẠCH NÀY
================================================================

1. Mở đoạn chat mới
2. Copy-paste TOÀN BỘ file này vào
3. Nói: "Làm bước N" (N = 1 đến 16)
4. AI sẽ viết code hoàn chỉnh cho bước đó
5. Review → confirm → bước tiếp theo
6. Nếu cần sửa: "Sửa bước N, vấn đề là..."
7. Bước sau có thể cần output bước trước
   → gửi kèm file đã tạo ở bước trước nếu cần
8. Bước 13-16 (web) có thể làm song song với firmware
9. KHÔNG CÒN Bước 17 — wifi_firebase.h/cpp đã hoàn chỉnh ở Bước 10

CÁC FILE ĐÃ HOÀN THIỆN (không cần làm lại):
  ✅ wifi_firebase.h / wifi_firebase.cpp  — unified stream v5.0

Không cần gửi code cũ. Mọi thứ cần biết đã nằm trong
bản kế hoạch này.