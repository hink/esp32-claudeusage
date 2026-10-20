#pragma once

#include <stdint.h>

// Builds the screen and starts its timers. Call with the LVGL lock held.
void ui_init(uint8_t brightness);
