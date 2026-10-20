// Board support for the Waveshare ESP32-C6-Touch-AMOLED-2.16.
// Pin map cross-checked against Waveshare's Arduino demo and XiaoZhi board config
// (the user_config.h in Waveshare's ESP-IDF demos has CS/INT swapped).

#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"

static const char *TAG = "board";

#define PIN_I2C_SDA   GPIO_NUM_8
#define PIN_I2C_SCL   GPIO_NUM_7
#define PIN_LCD_CS    GPIO_NUM_15
#define PIN_LCD_PCLK  GPIO_NUM_0
#define PIN_LCD_D0    GPIO_NUM_1
#define PIN_LCD_D1    GPIO_NUM_2
#define PIN_LCD_D2    GPIO_NUM_3
#define PIN_LCD_D3    GPIO_NUM_4
#define PIN_TP_RST    GPIO_NUM_11
#define PIN_TP_INT    GPIO_NUM_5
#define PIN_BOOT_BTN  GPIO_NUM_9

#define AXP2101_ADDR  0x34
#define LCD_BUF_LINES 40

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pmic;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;

// Same sequence as Waveshare's LVGL demo for this panel (CO5300, SH8601-compatible),
// except brightness (0x51) starts at 0 so random GRAM contents never show.
static const sh8601_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x30}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pmic, buf, sizeof(buf), 100);
}

static esp_err_t pmic_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pmic, &reg, 1, val, 1, 100);
}

static esp_err_t pmic_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t v = 0;
    esp_err_t err = pmic_read(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    return pmic_write(reg, (v & ~mask) | (bits & mask));
}

static esp_err_t pmic_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_pmic), TAG, "add pmic");

    // VBUS input current limit 2000 mA (as Waveshare's demo does).
    pmic_update(0x16, 0x07, 0x05);
    // DCDC1 = 3.3 V
    pmic_write(0x82, (3300 - 1500) / 100);
    // ALDO1..4 = 3.3 V, then enable them. ALDO3 feeds the panel.
    for (uint8_t reg = 0x92; reg <= 0x95; reg++) {
        pmic_write(reg, (3300 - 500) / 100);
    }
    return pmic_update(0x90, 0x0F, 0x0F);
}

static void pmic_aldo3(bool on)
{
    pmic_update(0x90, 0x04, on ? 0x04 : 0x00);
}

static void rounder_cb(lv_event_t *e)
{
    // The panel needs even start / odd end coordinates.
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static esp_err_t display_init(void)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        BOARD_LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_lcd_io),
                        TAG, "panel io");

    sh8601_vendor_config_t vendor = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_lcd_io, &panel_cfg, &s_panel), TAG, "panel");

    // No reset pin: power-cycle the panel via ALDO3 instead.
    pmic_aldo3(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    pmic_aldo3(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    pmic_aldo3(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    return esp_lcd_panel_init(s_panel);
}

static esp_err_t touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io), TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1},
    };
    return esp_lcd_touch_new_i2c_cst9217(tp_io, &tp_cfg, &s_touch);
}

esp_err_t board_init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c bus");

    if (pmic_init() != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 init failed; display may stay dark");
    }
    ESP_RETURN_ON_ERROR(display_init(), TAG, "display");

    if (imu_init(s_i2c) != ESP_OK) {
        ESP_LOGW(TAG, "no accelerometer; auto-rotate disabled");
    }

    bool have_touch = touch_init() == ESP_OK;
    if (!have_touch) {
        ESP_LOGW(TAG, "touch init failed; continuing without touch");
    }

    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_H_RES * LCD_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        // The panel can't swap X/Y in hardware, so rotate in software.
        .flags = {.buff_dma = true, .swap_bytes = true, .sw_rotate = true},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        return ESP_FAIL;
    }
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    if (have_touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = s_disp, .handle = s_touch};
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "could not register touch with LVGL");
        }
    }
    return ESP_OK;
}

void board_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint8_t v = (uint8_t)((percent * 255) / 100);
    esp_lcd_panel_io_tx_param(s_lcd_io, (0x02 << 24) | (0x51 << 8), &v, 1);
}

lv_display_t *board_display(void)
{
    return s_disp;
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(PIN_BOOT_BTN) == 0;
}
