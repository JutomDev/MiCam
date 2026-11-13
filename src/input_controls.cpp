#include "input_controls.h"
#include "config.h"

void Button::init(int8_t gpioPin) {
    pin = gpioPin;
    lastState = HIGH;
    isPressed = false;
    pressStartTime = 0;
    longPressFired = false;
    shortClicked = false;
    longClicked = false;
    if (pin >= 0) {
        pinMode(pin, INPUT_PULLUP);
    }
}

void Button::clearEvents() {
    shortClicked = false;
    longClicked = false;
}

void Button::update(uint32_t now) {
    shortClicked = false;
    longClicked = false;
    if (pin < 0) return;

    bool currentPinState = digitalRead(pin);

    if (lastState == HIGH && currentPinState == LOW) {
        isPressed = true;
        pressStartTime = now;
        longPressFired = false;
    } else if (currentPinState == LOW && isPressed) {
        if (!longPressFired && (now - pressStartTime >= LONG_PRESS_TIME_MS)) {
            longClicked = true;
            longPressFired = true;
        }
    } else if (lastState == LOW && currentPinState == HIGH) {
        if (isPressed) {
            uint32_t duration = now - pressStartTime;
            if (!longPressFired && (duration >= DEBOUNCE_TIME_MS)) {
                shortClicked = true;
            }
        }
        isPressed = false;
    }

    lastState = currentPinState;
}

void AnalogJoystick::init(int8_t px, int8_t py) {
    pinX = px;
    pinY = py;
    lastActionTime = 0;
    centerSinceTime = 0;
    pendingSince = 0;
    latchedDir = JOY_NONE;
    pendingDir = JOY_NONE;
    repeatCount = 0;
    waitForNeutral = false;
    if (pinX >= 0) pinMode(pinX, INPUT);
    if (pinY >= 0) pinMode(pinY, INPUT);
}

void AnalogJoystick::readAveraged(int& outX, int& outY) {
    int sumX = 0;
    int sumY = 0;
    for (int i = 0; i < 4; i++) {
        sumX += analogRead(pinX);
        sumY += analogRead(pinY);
    }
    outX = sumX >> 2;
    outY = sumY >> 2;
}

void AnalogJoystick::waitRelease() {
    waitForNeutral = true;
    latchedDir = JOY_NONE;
    pendingDir = JOY_NONE;
    pendingSince = 0;
    repeatCount = 0;
}

JoyDirection AnalogJoystick::getRawDirection() {
#if USE_ANALOG_JOYSTICK
    if (pinX < 0 || pinY < 0) return JOY_NONE;
    int xVal = 2048;
    int yVal = 2048;
    readAveraged(xVal, yVal);

    int dx = xVal - 2048;
    int dy = yVal - 2048;
    const int DEADZONE = 950;

    if (abs(dx) < DEADZONE && abs(dy) < DEADZONE) return JOY_NONE;
    if (abs(dy) >= abs(dx)) {
        return (dy < 0) ? JOY_UP : JOY_DOWN;
    }
    // Invert X axis so stick movement matches screen direction
    return (dx < 0) ? JOY_RIGHT : JOY_LEFT;
#else
    return JOY_NONE;
#endif
}

JoyDirection AnalogJoystick::getDirection(uint32_t now) {
#if USE_ANALOG_JOYSTICK
    if (pinX < 0 || pinY < 0) return JOY_NONE;
    int xVal = 2048;
    int yVal = 2048;
    readAveraged(xVal, yVal);

    // Schmitt trigger thresholds (12-bit ADC: 0..4095, center ~2048)
    const int TRIG_LOW = 800;
    const int TRIG_HIGH = 3300;
    const int REL_LOW = 1400;
    const int REL_HIGH = 2700;

    bool inNeutral = (xVal > REL_LOW && xVal < REL_HIGH && yVal > REL_LOW && yVal < REL_HIGH);

    if (inNeutral) {
        pendingDir = JOY_NONE;
        pendingSince = 0;
        if (centerSinceTime == 0) {
            centerSinceTime = now;
        } else if (now - centerSinceTime >= 65) {
            latchedDir = JOY_NONE;
            repeatCount = 0;
            waitForNeutral = false;
        }
        return JOY_NONE;
    }

    centerSinceTime = 0;
    if (waitForNeutral) {
        return JOY_NONE;
    }

    JoyDirection candidate = JOY_NONE;
    int dx = xVal - 2048;
    int dy = yVal - 2048;

    if (abs(dy) >= abs(dx)) {
        if (yVal < TRIG_LOW)
            candidate = JOY_UP;
        else if (yVal > TRIG_HIGH)
            candidate = JOY_DOWN;
    } else {
        if (xVal < TRIG_LOW)
            candidate = JOY_RIGHT;
        else if (xVal > TRIG_HIGH)
            candidate = JOY_LEFT;
    }

    if (candidate == JOY_NONE) {
        pendingDir = JOY_NONE;
        pendingSince = 0;
        return JOY_NONE;
    }

    // Require >= 20 ms continuous deflection to reject Wi-Fi/RF ADC glitches
    if (latchedDir == JOY_NONE) {
        if (pendingDir != candidate) {
            pendingDir = candidate;
            pendingSince = now;
            return JOY_NONE;
        }
        if (now - pendingSince < 20) {
            return JOY_NONE;
        }
        latchedDir = candidate;
        pendingDir = JOY_NONE;
        repeatCount = 0;
        lastActionTime = now;
        return candidate;
    }

    uint32_t repeatDelay = (repeatCount == 0) ? 550 : 350;
    if (candidate == latchedDir && (now - lastActionTime >= repeatDelay)) {
        if (repeatCount < 250) repeatCount++;
        lastActionTime = now;
        return candidate;
    }
#endif
    return JOY_NONE;
}
