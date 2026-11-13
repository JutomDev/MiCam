#pragma once

#include <Arduino.h>

/**
 * @brief Non-blocking mechanical push-button handler with debounce and long-press detection.
 */
struct Button {
    int8_t pin = -1;
    bool lastState = HIGH;
    bool isPressed = false;
    uint32_t pressStartTime = 0;
    bool longPressFired = false;
    bool shortClicked = false;
    bool longClicked = false;

    void init(int8_t gpioPin);
    void update(uint32_t now);
    void clearEvents();
};

enum JoyDirection { JOY_NONE = 0, JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT };

/**
 * @brief 2D analog joystick driver (ADC1) with Schmitt-trigger hysteresis,
 *        20 ms RF-spike rejection, and UI auto-repeat.
 */
struct AnalogJoystick {
    int8_t pinX = -1;
    int8_t pinY = -1;
    uint32_t lastActionTime = 0;
    uint32_t centerSinceTime = 0;
    uint32_t pendingSince = 0;
    JoyDirection latchedDir = JOY_NONE;
    JoyDirection pendingDir = JOY_NONE;
    uint8_t repeatCount = 0;
    bool waitForNeutral = false;

    void init(int8_t px, int8_t py);
    void waitRelease();
    JoyDirection getRawDirection();
    JoyDirection getDirection(uint32_t now);

private:
    void readAveraged(int& outX, int& outY);
};
