#include "button_manager.h"
#include "logger.h"

// ================================================================
// button_manager.cpp
// Intelligent Aquarium v4.0
// ================================================================

ButtonManager buttonManager;

// Pin mapping theo thứ tự BtnId
const uint8_t ButtonManager::_pins[BTN_COUNT] = {
    PIN_BTN_UP,      // BtnId::UP     (GPIO 33)
    PIN_BTN_DOWN,    // BtnId::DOWN   (GPIO 32)
    PIN_BTN_SELECT,  // BtnId::SELECT (GPIO 22)
    PIN_BTN_BACK,    // BtnId::BACK   (GPIO 25)
};

// ----------------------------------------------------------------
ButtonManager::ButtonManager() {
    for (uint8_t i = 0; i < BTN_COUNT; i++) {
        _states[i] = { true, true, false, 0 };
        // INPUT_PULLUP: không nhấn = HIGH = true
    }
}

// ----------------------------------------------------------------
void ButtonManager::begin() {
    for (uint8_t i = 0; i < BTN_COUNT; i++) {
        pinMode(_pins[i], INPUT_PULLUP);
        bool raw = (digitalRead(_pins[i]) == HIGH);
        _states[i].raw          = raw;
        _states[i].debounced    = raw;
        _states[i].pressedFlag  = false;
        _states[i].lastChangeMs = 0;
    }
    LOG_INFO("BTN", "ButtonManager init: UP=%d DOWN=%d SELECT=%d BACK=%d (debounce %dms)",
             PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_SELECT, PIN_BTN_BACK, (int)DEBOUNCE_MS);
}

// ================================================================
// UPDATE — gọi đầu mỗi loop()
// Đọc raw → debounce → detect falling edge (HIGH→LOW = nhấn)
// ================================================================
void ButtonManager::update() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < BTN_COUNT; i++) {
        BtnState& s = _states[i];

        bool rawNow = (digitalRead(_pins[i]) == HIGH);

        if (rawNow != s.raw) {
            s.raw          = rawNow;
            s.lastChangeMs = now;
        }

        if ((now - s.lastChangeMs) >= DEBOUNCE_MS) {
            bool prev   = s.debounced;
            s.debounced = s.raw;

            // Falling edge (true → false) = nút vừa nhấn
            if (prev && !s.debounced) {
                s.pressedFlag = true;
                static const char* names[] = { "UP", "DOWN", "SELECT", "BACK" };
                LOG_DEBUG("BTN", "Button %s pressed (GPIO %d)", names[i], _pins[i]);
            }
        }
    }
}

// ----------------------------------------------------------------
bool ButtonManager::wasPressed(BtnId id) {
    uint8_t i = (uint8_t)id;
    if (i >= BTN_COUNT) return false;
    if (_states[i].pressedFlag) {
        _states[i].pressedFlag = false;
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
