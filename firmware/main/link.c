#include "link.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu.h"
#include "orient.h"

#define FW_VERSION         "1.1.0"
#define LINK_LINE_MAX      1536
#define BRIDGE_TIMEOUT_US  (45LL * 1000 * 1000)
#define HELLO_EVERY_US     (5LL * 1000 * 1000)

static const char *TAG = "link";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_tx_lock;
static link_state_t s_state;
static int64_t s_epoch_at_sync;  // unix seconds
static int64_t s_mono_at_sync;   // esp_timer us
static int s_brightness_req = -1;
static volatile int64_t s_stream_until_us;

static void send_line(const char *json)
{
    if (!usb_serial_jtag_is_connected()) {
        return;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    usb_serial_jtag_write_bytes("\n@", 2, pdMS_TO_TICKS(20));
    usb_serial_jtag_write_bytes(json, strlen(json), pdMS_TO_TICKS(50));
    usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(20));
    xSemaphoreGive(s_tx_lock);
}

static void send_imu(void)
{
    int16_t a[3];
    if (!imu_read(a)) {
        send_line("{\"type\":\"imu\",\"error\":\"no accelerometer\"}");
        return;
    }
    int a0, d, flip;
    orient_get_cal(&a0, &d, &flip);
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"imu\",\"t\":%lld,\"x\":%d,\"y\":%d,\"z\":%d,\"up\":%d,\"rot\":%d,\"cal\":[%d,%d,%d]}",
             (long long)(esp_timer_get_time() / 1000), a[0], a[1], a[2], orient_classify(a),
             orient_current_rotation(), a0, d, flip);
    send_line(buf);
}

static int64_t json_i64(const cJSON *obj, const char *key, int64_t dflt)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? (int64_t)it->valuedouble : dflt;
}

static void json_str(const cJSON *obj, const char *key, char *dst, size_t len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strlcpy(dst, it->valuestring, len);
    } else {
        dst[0] = '\0';
    }
}

static void parse_window(const cJSON *root, const char *key, usage_window_t *w)
{
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, key);
    const cJSON *pct = cJSON_GetObjectItemCaseSensitive(obj, "pct");
    if (!cJSON_IsObject(obj) || !cJSON_IsNumber(pct)) {
        w->valid = false;
        w->pct = 0;
        w->resets_at = 0;
        return;
    }
    w->valid = true;
    w->pct = (float)pct->valuedouble;
    w->resets_at = json_i64(obj, "resets_at", 0);
}

static void handle_line(const char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line != '{') {
        return;
    }
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad json");
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.last_rx_us = now_us;

    int64_t now = json_i64(root, "now", 0);
    if (now > 1600000000) {
        s_epoch_at_sync = now;
        s_mono_at_sync = now_us;
        s_state.have_time = true;
    }

    if (strcmp(t, "state") == 0) {
        s_state.tz_offset = (int32_t)json_i64(root, "tz", s_state.tz_offset);
        json_str(root, "plan", s_state.plan, sizeof(s_state.plan));
        json_str(root, "status", s_state.status, sizeof(s_state.status));
        json_str(root, "detail", s_state.detail, sizeof(s_state.detail));
        s_state.next_poll_at = json_i64(root, "next_poll_at", 0);

        char notice[sizeof(s_state.notice)];
        json_str(root, "notice", notice, sizeof(notice));
        if (notice[0]) {
            strlcpy(s_state.notice, notice, sizeof(s_state.notice));
            s_state.notice_until_us = now_us + 6LL * 1000 * 1000;
        }

        int64_t fetched_at = json_i64(root, "fetched_at", 0);
        if (fetched_at > 0) {
            s_state.fetched_at = fetched_at;
            s_state.have_usage = true;
            parse_window(root, "five_hour", &s_state.five_hour);
            parse_window(root, "seven_day", &s_state.seven_day);
        }
    } else if (strcmp(t, "config") == 0) {
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "brightness");
        if (cJSON_IsNumber(b)) {
            s_brightness_req = b->valueint;
        }
    } else if (strcmp(t, "notice") == 0) {
        json_str(root, "text", s_state.notice, sizeof(s_state.notice));
        s_state.notice_until_us = now_us + json_i64(root, "secs", 6) * 1000000LL;
    } else if (strcmp(t, "imu_stream") == 0) {
        int64_t secs = json_i64(root, "secs", 30);
        s_stream_until_us = now_us + (secs > 120 ? 120 : secs) * 1000000LL;
    }
    s_state.version++;
    xSemaphoreGive(s_lock);

    if (strcmp(t, "imu") == 0) {
        send_imu();
    } else if (strcmp(t, "orient_cal") == 0) {
        orient_set_cal((int)json_i64(root, "a0", -2), (int)json_i64(root, "d", 0), (int)json_i64(root, "flip", 0));
        send_imu();
    }
    cJSON_Delete(root);
}

static void link_task(void *arg)
{
    static char line[LINK_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[256];
    int64_t last_hello = -HELLO_EVERY_US;
    int64_t last_stream = 0;

    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                if (!overflow) {
                    line[len] = '\0';
                    handle_line(line);
                }
                len = 0;
                overflow = false;
            } else if (c != '\r') {
                if (len < LINK_LINE_MAX - 1) {
                    line[len++] = c;
                } else {
                    overflow = true;
                }
            }
        }

        // Announce ourselves until the bridge talks to us, so a freshly
        // started board gets the cached state without a new API call.
        int64_t now_us = esp_timer_get_time();
        if (now_us < s_stream_until_us && now_us - last_stream >= 100000) {
            send_imu();
            last_stream = now_us;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t last_rx = s_state.last_rx_us;
        xSemaphoreGive(s_lock);
        bool quiet = last_rx == 0 || now_us - last_rx > 15LL * 1000 * 1000;
        if (quiet && now_us - last_hello > HELLO_EVERY_US) {
            send_line("{\"type\":\"hello\",\"fw\":\"" FW_VERSION "\"}");
            last_hello = now_us;
        }
    }
}

void link_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    strlcpy(s_state.status, "starting", sizeof(s_state.status));

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 2048;
    cfg.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();

    xTaskCreate(link_task, "link", 6144, NULL, 5, NULL);
}

void link_get_state(link_state_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

int64_t link_now(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = 0;
    if (s_state.have_time) {
        now = s_epoch_at_sync + (esp_timer_get_time() - s_mono_at_sync) / 1000000;
    }
    xSemaphoreGive(s_lock);
    return now;
}

bool link_bridge_alive(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t last = s_state.last_rx_us;
    xSemaphoreGive(s_lock);
    return last != 0 && esp_timer_get_time() - last < BRIDGE_TIMEOUT_US;
}

void link_request_refresh(void)
{
    send_line("{\"type\":\"refresh\"}");
}

int link_take_brightness_request(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int b = s_brightness_req;
    s_brightness_req = -1;
    xSemaphoreGive(s_lock);
    return b;
}
