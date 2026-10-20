// Claude usage display for the Waveshare ESP32-C6-Touch-AMOLED-2.16.
// The PC bridge (bridge/claude_usage_bridge.py) polls Anthropic's usage endpoint
// and streams the numbers over USB; this firmware only draws them.

#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "orient.h"
#include "esp_heap_caps.h"
#include "ui.h"

static const char *TAG = "main";

#define DEFAULT_BRIGHTNESS 70

static uint8_t load_brightness(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    uint8_t b = DEFAULT_BRIGHTNESS;
    nvs_handle_t h;
    if (err == ESP_OK && nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bright", &b);
        nvs_close(h);
    }
    return b > 100 ? DEFAULT_BRIGHTNESS : b;
}

void app_main(void)
{
    uint8_t brightness = load_brightness();

    ESP_ERROR_CHECK(board_init());
    link_init();

    lvgl_port_lock(0);
    ui_init(brightness);
    orient_init(board_display());
    lvgl_port_unlock();

    // Let the first frame land before lighting the panel.
    vTaskDelay(pdMS_TO_TICKS(300));
    lvgl_port_lock(0);
    board_set_brightness(brightness);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "up, brightness %u%%, free heap %u (min %u)", brightness,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    esp_log_level_set("*", ESP_LOG_WARN);
}
