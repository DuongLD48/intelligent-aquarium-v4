#pragma once
#include "type_definitions.h"
#include "system_constants.h"
#include <Arduino.h>

// ================================================================
// water_change_manager.h
// Intelligent Aquarium v4.0 — MODULE MỚI (bước 7.5)
//
// Quản lý quy trình thay nước KHÔNG dùng cảm biến mực nước:
//   - Tự động theo lịch NTP (giờ:phút mỗi ngày)
//   - Thủ công qua Firebase trigger hoặc nút bấm vật lý
//
// Luồng state machine:
//   IDLE → PUMPING_OUT → PUMPING_IN → DONE → IDLE
//
// Safety Core vẫn kiểm tra mutual exclusion pump_in + pump_out.
// ================================================================

// ----------------------------------------------------------------
// WATER CHANGE CONFIG — cấu hình lịch và thời gian bơm
// ----------------------------------------------------------------
struct WaterChangeConfig {
    bool     schedule_enabled = false;  // Bật/tắt lịch tự động
    uint8_t  schedule_hour    = 8;      // Giờ chạy (0-23)
    uint8_t  schedule_minute  = 0;      // Phút chạy (0-59)
    uint16_t pump_out_sec     = WATER_CHANGE_DEFAULT_PUMP_OUT_SEC;  // Giây bơm ra
    uint16_t pump_in_sec      = WATER_CHANGE_DEFAULT_PUMP_IN_SEC;   // Giây bơm vào
};

// ----------------------------------------------------------------
// WATER CHANGE MANAGER
// ----------------------------------------------------------------
class WaterChangeManager {
public:
    WaterChangeManager();

    // Gọi trong setup(): khởi tạo state
    void begin();

    // Cập nhật config từ Firebase / Serial
    void setConfig(const WaterChangeConfig& cfg);

    // Gọi mỗi iteration loop() — kiểm tra lịch và chạy state machine
    void update();

    // Kích hoạt thay nước thủ công (từ Firebase trigger hoặc nút bấm)
    // Bị ignore nếu đang bận (isBusy() == true)
    void triggerManual();

    // Đang trong quá trình thay nước?
    bool isBusy() const;

    // Trạng thái hiện tại
    WaterChangeState getState() const { return _state; }

    // Lấy lệnh relay (để merge vào RelayCommand trong main.cpp)
    void getRelayCmd(bool& pump_out, bool& pump_in) const;

    // JSON status để upload Firebase
    // {"state":"IDLE","last_run_day":20250311,"busy":false}
    String getStatusJson() const;

    // Getters
    const WaterChangeConfig& getConfig()    const { return _cfg; }
    uint32_t                 lastRunDay()   const { return _lastRunDay; }

private:
    WaterChangeConfig _cfg;
    WaterChangeState  _state;
    unsigned long     _stateStartMs;  // millis() khi bước vào state hiện tại
    uint32_t          _lastRunDay;    // epoch/86400 — tránh chạy 2 lần/ngày
    bool              _manualTrigger; // Cờ kích hoạt thủ công

    // Chạy state machine 1 tick
    void _tick();

    // Kiểm tra xem giờ NTP hiện tại có khớp lịch không
    // Trả true chỉ khi NTP sync OK + giờ:phút đúng + chưa chạy hôm nay
    bool _isScheduleTime() const;

    // Lấy ngày hiện tại dạng epoch/86400 (UTC+7)
    uint32_t _todayDay() const;

    // Chuyển state, ghi log
    void _setState(WaterChangeState newState);

    // Tên state để log
    static const char* _stateName(WaterChangeState s);
};

// Global singleton
extern WaterChangeManager waterChangeManager;
