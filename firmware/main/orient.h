#pragma once

#include <stdint.h>
#include "lvgl.h"

// Auto-rotation from the accelerometer.
//
// Gravity's in-plane direction picks one of four "up" axes in sensor space:
// 0 = +X, 1 = +Y, 2 = -X, 3 = -Y (or -1 when lying flat / on a diagonal).
// A one-time calibration maps that to a display rotation:
//   a0   = up axis when the UI reads correctly at rotation 0
//   d    = 1 or 3, how the axis index steps when the board turns 90 deg clockwise
//   flip = swap the 90/270 rotations if the panel turns the other way
// Until calibrated the display stays at rotation 0.

// Call with the LVGL lock held, after the UI exists.
void orient_init(lv_display_t *disp);

int orient_classify(const int16_t accel[3]);
int orient_current_rotation(void);  // 0..3 (x 90 deg)
void orient_get_cal(int *a0, int *d, int *flip);

// Thread-safe. a0 = -1 clears the calibration. Persists to NVS.
void orient_set_cal(int a0, int d, int flip);
