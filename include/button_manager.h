#pragma once
#include "system_constants.h"
#include <Arduino.h>

// ================================================================
// button_manager.h
// Intelligent Aquarium v4.0
//
// Quản lý 6 nút bấm với debounce 50ms.
// wasPressed(id) → true đúng 1 lần sau khi nhấn (edge detect).
//
// Nút:
//   BTN_PAGE         → GPIO 25
//   BTN_UP           → GPIO 33
//   BTN_DOWN         → GPIO 32
//   BTN_SELECT       → GPIO 22  (đổi từ 15, tránh trùng OLED DC)
//   BTN_BACK         → GPIO 0   (boot pin, cẩn thận)
//   BTN_WATER_CHANGE → GPIO 2   (kích hoạt thay nước thủ công)
// ================================================================

// ----------------------------------------------------------------
// BUTTON ID ENUM
// ----------------------------------------------------------------
enum class BtnId : uint8_t {
    PAGE         = 0,
    UP           = 1,
    DOWN         = 2,
    SELECT       = 3,
    BACK         = 4,
    WATER_CHANGE = 5,
    COUNT        = 6
};

// ----------------------------------------------------------------
// BUTTON MANAGER
// ----------------------------------------------------------------
class ButtonManager {
public:
    ButtonManager();

    // Gọi trong setup(): pinMode INPUT_PULLUP cho cả 6 nút
    void begin();

    // Gọi đầu mỗi loop(): cập nhật trạng thái và debounce
    void update();

    // Trả true đúng 1 lần sau khi nút được nhấn (falling edge)
    // Tự reset flag sau khi đọc
    bool wasPressed(BtnId id);

    // Kiểm tra nút đang giữ (raw, không debounce)
    bool isHeld(BtnId id) const;

private:
    static constexpr uint8_t  BTN_COUNT      = (uint8_t)BtnId::COUNT;
    static constexpr uint32_t DEBOUNCE_MS    = 50;

    // GPIO pin tương ứng với từng BtnId
    static const uint8_t _pins[BTN_COUNT];

    struct BtnState {
        bool     raw;          // Trạng thái raw hiện tại (LOW = nhấn)
        bool     debounced;    // Trạng thái sau debounce
        bool     lastDebounced;// Trạng thái debounced chu kỳ trước
        bool     pressedFlag;  // Flag "vừa nhấn" — đọc 1 lần rồi reset
        uint32_t lastChangeMs; // millis() lúc raw thay đổi
    };

    BtnState _states[BTN_COUNT];
};

// Global singleton
extern ButtonManager buttonManager;
