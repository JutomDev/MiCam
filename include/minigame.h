#pragma once

#include <Arduino.h>
#include "input_controls.h"

/**
 * @brief Retro arcade mini-game ("Mii-Snake") rendered incrementally on ST7789.
 */
class MiniGame {
public:
    static void init();
    // Returns true when the player requests to exit back to the settings menu.
    static bool update(uint32_t now, AnalogJoystick& joystick, const Button& btnShutter);
};
