#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 480
#define BOARD_LCD_V_RES 480

// Brings up I2C, the AXP2101 power chip, the AMOLED panel, touch and LVGL.
// The panel starts dark; call board_set_brightness() once the UI is drawn.
esp_err_t board_init(void);

// 0-100. Must be called with the LVGL port lock held.
void board_set_brightness(uint8_t percent);

lv_display_t *board_display(void);

// True while the BOOT button (GPIO9) is held down.
bool board_boot_button_pressed(void);
