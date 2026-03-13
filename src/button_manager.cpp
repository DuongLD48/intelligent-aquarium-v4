#include "button_manager.h"
#include "logger.h"

// ================================================================
// button_manager.cpp
// Intelligent Aquarium v4.0
// ================================================================

// Global singleton
ButtonManager buttonManager;

// Pin mapping theo thứ tự BtnId
const uint8_t ButtonManager::_pins[BTN_COUNT] = {
    PIN_BTN_PAGE,          // BtnId::PAGE
    PIN_BTN_UP,            // BtnId::UP
    PIN_BTN_DOWN,          // BtnId::DOWN
    PIN_BTN_SELECT,        // BtnId::SELECT  (GPIO 22)
    PIN_BTN_BACK,          // BtnId::BACK    (GPIO 0)
    PIN_BTN_WATER_CHANGE,  // BtnId::WATER_CHANGE (GPIO 2)
};

// ----------------------------------------------------------------
ButtonManager::ButtonManager() {
    for (uint8_t i = 0; i < BTN_COUNT; i++) {
        _states[i] = { true, true, true, false, 0 };
        // INPUT_PULLUP: không nhấn = HIGH = true
    }
}

// ----------------------------------------------------------------
void ButtonManager::begin() {
    for (uint8_t i = 0; i < BTN_COUNT; i++) {
        pinMode(_pins[i], INPUT_PULLUP);
        // Đọc trạng thái ban đầu
        bool raw = (digitalRead(_pins[i]) == HIGH);
        _states[i].raw          = raw;
        _states[i].debounced    = raw;
        _states[i].lastDebounced = raw;
        _states[i].pressedFlag  = false;
        _states[i].lastChangeMs = 0;
    }
    LOG_INFO("BTN", "ButtonManager init: 6 buttons (debounce %dms)", (int)DEBOUNCE_MS);
}

// ================================================================
// UPDATE — gọi đầu mỗi loop()
// Đọc raw → debounce → detect falling edge (HIGH→LOW = nhấn)
// ================================================================
void ButtonManager::update() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < BTN_COUNT; i++) {
        BtnState& s = _states[i];

        // Đọc raw: INPUT_PULLUP → nhấn = LOW → raw = false
        bool rawNow = (digitalRead(_pins[i]) == HIGH);

        // Nếu raw thay đổi → reset debounce timer
        if (rawNow != s.raw) {
            s.raw          = rawNow;
            s.lastChangeMs = now;
        }

        // Chỉ cập nhật debounced sau khi raw ổn định >= DEBOUNCE_MS
        if ((now - s.lastChangeMs) >= DEBOUNCE_MS) {
            bool prevDebounced = s.debounced;
            s.debounced = s.raw;

            // Falling edge (true → false) = nút vừa được nhấn
            if (prevDebounced && !s.debounced) {
                s.pressedFlag = true;
                LOG_DEBUG("BTN", "Button %d pressed (GPIO %d)", i, _pins[i]);
            }
        }
    }
}

// ----------------------------------------------------------------
bool ButtonManager::wasPressed(BtnId id) {
    uint8_t i = (uint8_t)id;
    if (i >= BTN_COUNT) return false;

    if (_states[i].pressedFlag) {
        _states[i].pressedFlag = false;  // Reset sau khi đọc
        return true;
    }
    return false;
}

// ----------------------------------------------------------------
bool ButtonManager::isHeld(BtnId id) const {
    uint8_t i = (uint8_t)id;
    if (i >= BTN_COUNT) return false;
    return !_states[i].debounced;  // LOW = đang nhấn
}
