#pragma once
#include "system_constants.h"
#include <Arduino.h>

// ================================================================
// button_manager.h
// Intelligent Aquarium v4.0
//
// Quản lý 4 nút điều hướng kiểu menu điện thoại đời cũ:
//   UP     (GPIO 33) → di chuyển lên  ↑
//   DOWN   (GPIO 32) → di chuyển xuống ↓
//   SELECT (GPIO 22) → vào menu / xác nhận
//   BACK   (GPIO 25) → quay lại màn hình trước
//
// wasPressed(id) → true đúng 1 lần sau khi nhấn (edge detect).
// Debounce 50ms.
// ================================================================

// ----------------------------------------------------------------
// BUTTON ID ENUM
// ----------------------------------------------------------------
enum class BtnId : uint8_t {
    UP     = 0,
    DOWN   = 1,
    SELECT = 2,
    BACK   = 3,
    COUNT  = 4
};

// ----------------------------------------------------------------
// BUTTON MANAGER
// ----------------------------------------------------------------
class ButtonManager {
public:
    ButtonManager();

    // Gọi trong setup(): pinMode INPUT_PULLUP cho 4 nút
    void begin();

    // Gọi đầu mỗi loop(): cập nhật trạng thái và debounce
    void update();

    // Trả true đúng 1 lần sau khi nút được nhấn (falling edge)
    bool wasPressed(BtnId id);

    // Kiểm tra nút đang giữ (debounced)
    bool isHeld(BtnId id) const;

private:
    static constexpr uint8_t  BTN_COUNT   = (uint8_t)BtnId::COUNT;
    static constexpr uint32_t DEBOUNCE_MS = 50;

    static const uint8_t _pins[BTN_COUNT];

    struct BtnState {
        bool     raw;
        bool     debounced;
        bool     pressedFlag;
        uint32_t lastChangeMs;
    };

    BtnState _states[BTN_COUNT];
};

// Global singleton
extern ButtonManager buttonManager;
