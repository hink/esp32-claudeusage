#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// QMI8658 accelerometer, +-2 g range: 16384 counts per g.
#define IMU_ONE_G 16384

esp_err_t imu_init(i2c_master_bus_handle_t bus);
bool imu_available(void);
// Raw accelerometer counts (x, y, z). Thread-safe.
bool imu_read(int16_t out[3]);
