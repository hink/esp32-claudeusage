// Minimal QMI8658 driver: accelerometer only, polled.
// Register values match Waveshare's qmi8658 component.

#include "imu.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define REG_WHO_AM_I 0x00  // reads 0x05
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03
#define REG_CTRL7    0x08
#define REG_AX_L     0x35

static i2c_master_dev_handle_t s_dev;
static bool s_ok;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, 50);
}

static esp_err_t probe(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add");
    uint8_t reg = REG_WHO_AM_I, id = 0;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 50);
    if (err == ESP_OK && id == 0x05) {
        return ESP_OK;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t imu_init(i2c_master_bus_handle_t bus)
{
    if (probe(bus, 0x6B) != ESP_OK && probe(bus, 0x6A) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 not found");
        return ESP_ERR_NOT_FOUND;
    }
    // The chip stays powered across ESP32 resets and ignores range changes
    // while running, so stop the sensors before configuring them.
    ESP_RETURN_ON_ERROR(wr(REG_CTRL7, 0x00), TAG, "ctrl7 off");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(wr(REG_CTRL1, 0x60), TAG, "ctrl1");  // register auto-increment
    ESP_RETURN_ON_ERROR(wr(REG_CTRL2, 0x07), TAG, "ctrl2");  // +-2 g, 62.5 Hz
    ESP_RETURN_ON_ERROR(wr(REG_CTRL7, 0x01), TAG, "ctrl7");  // accel on, gyro off
    vTaskDelay(pdMS_TO_TICKS(40));

    uint8_t reg = REG_CTRL2, ctrl2 = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &ctrl2, 1, 50) != ESP_OK || ctrl2 != 0x07) {
        ESP_LOGW(TAG, "CTRL2 reads 0x%02x, expected 0x07", ctrl2);
    }
    s_ok = true;
    ESP_LOGI(TAG, "QMI8658 ready");
    return ESP_OK;
}

bool imu_available(void)
{
    return s_ok;
}

bool imu_read(int16_t out[3])
{
    if (!s_ok) {
        return false;
    }
    uint8_t reg = REG_AX_L, b[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), 50) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out[i] = (int16_t)((b[2 * i + 1] << 8) | b[2 * i]);
    }
    return true;
}
