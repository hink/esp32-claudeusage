#include "orient.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "imu.h"
#include "nvs.h"

static const char *TAG = "orient";

#define SAMPLE_MS       100
#define STABLE_SAMPLES  5    // ~0.5 s steady before rotating

typedef struct {
    int a0;
    int d;
    int flip;
} cal_t;

static lv_display_t *s_disp;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static cal_t s_cal = {-1, 1, 0};
static volatile bool s_cal_changed;
static volatile int s_rotation;
static int s_candidate = -1;
static int s_count;
static int s_applied_up = -1;

static cal_t get_cal(void)
{
    taskENTER_CRITICAL(&s_mux);
    cal_t c = s_cal;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

static void nvs_save(const char *key, int8_t v)
{
    nvs_handle_t h;
    if (nvs_open("orient", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void set_rotation(int rot)
{
    s_rotation = rot;
    if ((int)lv_display_get_rotation(s_disp) != rot) {
        lv_display_set_rotation(s_disp, (lv_display_rotation_t)rot);
        nvs_save("rot", (int8_t)rot);
        ESP_LOGI(TAG, "rotation -> %d", rot * 90);
    }
}

static int rotation_for(int up, cal_t c)
{
    int k = ((((up - c.a0) % 4) + 4) % 4 * c.d) % 4;
    return c.flip ? (4 - k) % 4 : k;
}

int orient_classify(const int16_t a[3])
{
    int64_t ax = a[0], ay = a[1], az = a[2];
    int64_t inplane = ax * ax + ay * ay;
    int64_t total = inplane + az * az;
    // Relative to total gravity so the sensor's range setting doesn't matter.
    // Needs the screen tilted at least ~37 deg from lying flat.
    if (total == 0 || inplane * 100 < total * 36) {
        return -1;
    }
    int64_t aax = ax < 0 ? -ax : ax;
    int64_t aay = ay < 0 ? -ay : ay;
    // One axis must clearly win (1.4x, about 10 deg away from the diagonal).
    if (aax * 10 > aay * 14) {
        return ax > 0 ? 0 : 2;
    }
    if (aay * 10 > aax * 14) {
        return ay > 0 ? 1 : 3;
    }
    return -1;
}

static void tick(lv_timer_t *t)
{
    (void)t;
    cal_t c = get_cal();
    if (s_cal_changed) {
        s_cal_changed = false;
        s_applied_up = -1;
        if (c.a0 < 0) {
            set_rotation(0);
        }
    }

    int16_t a[3];
    if (!imu_read(a)) {
        return;
    }
    int up = orient_classify(a);
    if (up < 0) {
        s_candidate = -1;  // flat or ambiguous: keep whatever we had
        s_count = 0;
        return;
    }
    s_count = (up == s_candidate) ? s_count + 1 : 1;
    s_candidate = up;
    if (c.a0 >= 0 && s_count >= STABLE_SAMPLES && up != s_applied_up) {
        s_applied_up = up;
        set_rotation(rotation_for(up, c));
    }
}

void orient_init(lv_display_t *disp)
{
    s_disp = disp;
    int8_t a0 = -1, d = 1, flip = 0, rot = 0;
    nvs_handle_t h;
    if (nvs_open("orient", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i8(h, "a0", &a0);
        nvs_get_i8(h, "d", &d);
        nvs_get_i8(h, "flip", &flip);
        nvs_get_i8(h, "rot", &rot);
        nvs_close(h);
    }
    if (a0 >= 0 && a0 <= 3 && (d == 1 || d == 3)) {
        s_cal = (cal_t){a0, d, flip ? 1 : 0};
        set_rotation(rot & 3);
        // Snap straight to the current pose at boot instead of waiting.
        int16_t a[3];
        if (imu_read(a)) {
            int up = orient_classify(a);
            if (up >= 0) {
                s_applied_up = up;
                set_rotation(rotation_for(up, s_cal));
            }
        }
    }
    ESP_LOGI(TAG, "cal a0=%d d=%d flip=%d, rotation %d", s_cal.a0, s_cal.d, s_cal.flip, s_rotation * 90);
    lv_timer_create(tick, SAMPLE_MS, NULL);
}

int orient_current_rotation(void)
{
    return s_rotation;
}

void orient_get_cal(int *a0, int *d, int *flip)
{
    cal_t c = get_cal();
    *a0 = c.a0;
    *d = c.d;
    *flip = c.flip;
}

void orient_set_cal(int a0, int d, int flip)
{
    if (a0 < -1 || a0 > 3 || (d != 1 && d != 3)) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_cal = (cal_t){a0, d, flip ? 1 : 0};
    taskEXIT_CRITICAL(&s_mux);
    nvs_save("a0", (int8_t)a0);
    nvs_save("d", (int8_t)d);
    nvs_save("flip", (int8_t)(flip ? 1 : 0));
    s_cal_changed = true;
}
