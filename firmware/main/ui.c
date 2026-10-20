#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "crab.h"
#include "esp_timer.h"
#include "link.h"
#include "lvgl.h"
#include "nvs.h"

#define MARGIN       36
#define BAR_H        28

#define COL_TEXT     0xF2EFEA
#define COL_MUTED    0x8C8781
#define COL_DIM      0x5C5853
#define COL_TRACK    0x23211F
#define COL_ORANGE   0xD97757
#define COL_AMBER    0xE3A53C
#define COL_RED      0xE5534B
#define COL_GREEN    0x5DBB63

#define LATE_SECS    120        // a scheduled poll this overdue turns the dot amber
#define STALE_SECS   (30 * 60)  // ...and this overdue turns it red

typedef struct {
    lv_obj_t *title;
    lv_obj_t *pct;
    lv_obj_t *bar;
    lv_obj_t *sub;
} meter_t;

static lv_obj_t *s_root;
static lv_obj_t *s_plan;
static lv_obj_t *s_dot;
static lv_obj_t *s_ago;
static lv_obj_t *s_detail;
static meter_t s_m5;
static meter_t s_m7;

static uint8_t s_brightness;
static bool s_btn_was_down;
static bool s_refreshing;
static int64_t s_refresh_started_us;
static int64_t s_refresh_fetched_at;
static int s_shift;

static const uint8_t BRIGHTNESS_STEPS[] = {100, 70, 40, 15};

// ---------------------------------------------------------------- helpers

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_color(lv_obj_t *obj, uint32_t color, lv_part_t part)
{
    lv_color_t c = lv_color_hex(color);
    lv_obj_set_style_text_color(obj, c, part);
}

static void fmt_dur(char *out, size_t len, int64_t secs)
{
    if (secs < 0) {
        secs = 0;
    }
    if (secs < 60) {
        snprintf(out, len, "<1m");
    } else if (secs < 3600) {
        snprintf(out, len, "%dm", (int)(secs / 60));
    } else if (secs < 86400) {
        snprintf(out, len, "%dh %dm", (int)(secs / 3600), (int)((secs % 3600) / 60));
    } else {
        snprintf(out, len, "%dd %dh", (int)(secs / 86400), (int)((secs % 86400) / 3600));
    }
}

static void fmt_clock(char *out, size_t len, int64_t epoch, int32_t tz, bool weekday)
{
    static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int64_t local = epoch + tz;
    int day = (int)(((local / 86400) + 4) % 7);  // 1970-01-01 was a Thursday
    int secs = (int)(local % 86400);
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    int h12 = h % 12 == 0 ? 12 : h % 12;
    if (weekday) {
        snprintf(out, len, "%s %d:%02d %s", DAYS[day], h12, m, h < 12 ? "AM" : "PM");
    } else {
        snprintf(out, len, "%d:%02d %s", h12, m, h < 12 ? "AM" : "PM");
    }
}

static uint32_t level_color(float pct)
{
    if (pct >= 90) {
        return COL_RED;
    }
    if (pct >= 75) {
        return COL_AMBER;
    }
    return COL_ORANGE;
}

// ---------------------------------------------------------------- meters

static void meter_create(meter_t *m, const char *title, int y)
{
    m->title = make_label(s_root, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_text(m->title, title);
    lv_obj_set_pos(m->title, MARGIN, y + 17);

    m->pct = make_label(s_root, &lv_font_montserrat_40, COL_TEXT);
    lv_label_set_text(m->pct, "--");
    lv_obj_align(m->pct, LV_ALIGN_TOP_RIGHT, -MARGIN, y);

    m->bar = lv_bar_create(s_root);
    lv_obj_set_size(m->bar, BOARD_LCD_H_RES - 2 * MARGIN, BAR_H);
    lv_obj_set_pos(m->bar, MARGIN, y + 56);
    lv_obj_remove_flag(m->bar, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(m->bar, 0, 1000);
    lv_obj_set_style_bg_color(m->bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(m->bar, BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m->bar, lv_color_hex(COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(m->bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(m->bar, 700, LV_PART_MAIN);

    m->sub = make_label(s_root, &lv_font_montserrat_16, COL_MUTED);
    lv_obj_set_pos(m->sub, MARGIN, y + 94);
}

static float meter_update(meter_t *m, const usage_window_t *w, bool have_usage, int64_t now, int32_t tz,
                          bool weekly)
{
    char pct[16];
    char sub[80];
    char when[24];
    char clock[24];
    int32_t value = 0;
    uint32_t color = COL_ORANGE;
    float shown = 0;

    if (!have_usage || !w->valid) {
        snprintf(pct, sizeof(pct), "--");
        snprintf(sub, sizeof(sub), have_usage ? "No data for this limit" : "Waiting for data");
        set_color(m->pct, COL_DIM, 0);
    } else if (w->resets_at && now && now >= w->resets_at) {
        snprintf(pct, sizeof(pct), "0%%");
        snprintf(sub, sizeof(sub), "Limit reset - updating soon");
        set_color(m->pct, COL_TEXT, 0);
    } else {
        shown = w->pct < 0 ? 0 : (w->pct > 100 ? 100 : w->pct);
        if (shown > 0 && shown < 1) {
            snprintf(pct, sizeof(pct), "<1%%");
        } else {
            snprintf(pct, sizeof(pct), "%d%%", (int)(shown + 0.5f));
        }
        value = (int32_t)(shown * 10);
        color = level_color(shown);
        set_color(m->pct, shown >= 75 ? color : COL_TEXT, 0);
        if (w->resets_at && now) {
            fmt_dur(when, sizeof(when), w->resets_at - now);
            fmt_clock(clock, sizeof(clock), w->resets_at, tz, weekly);
            snprintf(sub, sizeof(sub), "Resets in %s  (%s)", when, clock);
        } else {
            snprintf(sub, sizeof(sub), "Reset time unknown");
        }
    }

    set_text(m->pct, pct);
    set_text(m->sub, sub);
    lv_obj_set_style_bg_color(m->bar, lv_color_hex(color), LV_PART_INDICATOR);
    if (lv_bar_get_value(m->bar) != value) {
        lv_bar_set_value(m->bar, value, LV_ANIM_ON);
    }
    return shown;
}

// ---------------------------------------------------------------- footer

static void footer_update(const link_state_t *st, int64_t now, bool alive)
{
    char ago[64];
    char detail[96] = "";
    char dur[24];
    uint32_t dot = COL_DIM;
    uint32_t detail_color = COL_DIM;
    int64_t now_us = esp_timer_get_time();

    if (!st->have_time) {
        snprintf(ago, sizeof(ago), "Waiting for PC bridge");
    } else if (!st->have_usage) {
        snprintf(ago, sizeof(ago), "No usage data yet");
    } else {
        int64_t age = now - st->fetched_at;
        if (age < 15) {
            snprintf(ago, sizeof(ago), "Updated just now");
        } else if (age < 60) {
            snprintf(ago, sizeof(ago), "Updated %ds ago", (int)age);
        } else {
            fmt_dur(dur, sizeof(dur), age);
            snprintf(ago, sizeof(ago), "Updated %s ago", dur);
        }
        // The bridge polls less often while Claude Code is idle, so judge
        // freshness by whether it's keeping to its own schedule, not by age.
        int64_t overdue = st->next_poll_at ? now - st->next_poll_at : age - 11 * 60;
        dot = overdue <= LATE_SECS ? COL_GREEN : overdue <= STALE_SECS ? COL_AMBER : COL_RED;
    }

    if (s_refreshing) {
        bool got_new = st->fetched_at != s_refresh_fetched_at;
        bool timed_out = now_us - s_refresh_started_us > 10LL * 1000 * 1000;
        bool has_notice = st->notice[0] && now_us < st->notice_until_us;
        if (got_new || timed_out || has_notice) {
            s_refreshing = false;
        }
    }

    if (s_refreshing) {
        snprintf(detail, sizeof(detail), "Refreshing...");
        detail_color = COL_MUTED;
    } else if (st->notice[0] && now_us < st->notice_until_us) {
        snprintf(detail, sizeof(detail), "%s", st->notice);
        detail_color = COL_MUTED;
    } else if (!alive) {
        snprintf(detail, sizeof(detail), st->have_time ? "PC bridge disconnected" : "Start claude-usage-bridge on your PC");
        detail_color = st->have_time ? COL_AMBER : COL_DIM;
    } else if (strcmp(st->status, "rate_limited") == 0) {
        fmt_dur(dur, sizeof(dur), st->next_poll_at - now);
        snprintf(detail, sizeof(detail), "Rate limited - next try in %s", dur);
        detail_color = COL_AMBER;
    } else if (strcmp(st->status, "auth") == 0) {
        snprintf(detail, sizeof(detail), "Login expired - open Claude Code");
        detail_color = COL_RED;
    } else if (strcmp(st->status, "offline") == 0) {
        snprintf(detail, sizeof(detail), "Can't reach Anthropic - retrying");
        detail_color = COL_AMBER;
    } else if (strcmp(st->status, "error") == 0) {
        snprintf(detail, sizeof(detail), "%s", st->detail[0] ? st->detail : "Bridge error");
        detail_color = COL_RED;
    } else if (st->next_poll_at > now) {
        fmt_dur(dur, sizeof(dur), st->next_poll_at - now);
        snprintf(detail, sizeof(detail), "Next update in %s - tap to refresh", dur);
    }

    if (strcmp(st->status, "auth") == 0 || strcmp(st->status, "error") == 0) {
        dot = COL_RED;
    } else if ((strcmp(st->status, "rate_limited") == 0 || strcmp(st->status, "offline") == 0) && dot == COL_GREEN) {
        dot = COL_AMBER;
    }

    set_text(s_ago, ago);
    set_text(s_detail, detail);
    set_color(s_detail, detail_color, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(dot), 0);
}

// ---------------------------------------------------------------- timers / input

static void save_brightness(uint8_t b)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", b);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void apply_brightness(uint8_t b, bool persist)
{
    s_brightness = b;
    board_set_brightness(b);
    if (persist) {
        save_brightness(b);
    }
}

static void ui_tick(lv_timer_t *t)
{
    (void)t;
    link_state_t st;
    link_get_state(&st);
    int64_t now = link_now();
    bool alive = link_bridge_alive();

    int req = link_take_brightness_request();
    if (req >= 0 && req <= 100 && req != s_brightness) {
        apply_brightness((uint8_t)req, true);
    }

    if (st.plan[0]) {
        char plan[48];
        snprintf(plan, sizeof(plan), "CLAUDE %s", st.plan);
        for (char *p = plan; *p; p++) {
            if (*p >= 'a' && *p <= 'z') {
                *p -= 32;
            }
        }
        set_text(s_plan, plan);
    }

    float p5 = meter_update(&s_m5, &st.five_hour, st.have_usage, now, st.tz_offset, false);
    float p7 = meter_update(&s_m7, &st.seven_day, st.have_usage, now, st.tz_offset, true);
    footer_update(&st, now, alive);

    float worst = p5 > p7 ? p5 : p7;
    if (!alive) {
        crab_set_mood(CRAB_SLEEP);
    } else if (worst >= 95) {
        crab_set_mood(CRAB_PANIC);
    } else if (worst >= 75) {
        crab_set_mood(CRAB_WORRIED);
    } else {
        crab_set_mood(CRAB_NORMAL);
    }
}

static void button_tick(lv_timer_t *t)
{
    (void)t;
    bool down = board_boot_button_pressed();
    if (down && !s_btn_was_down) {
        size_t n = sizeof(BRIGHTNESS_STEPS);
        size_t next = 0;
        for (size_t i = 0; i < n; i++) {
            if (BRIGHTNESS_STEPS[i] == s_brightness) {
                next = (i + 1) % n;
                break;
            }
        }
        apply_brightness(BRIGHTNESS_STEPS[next], true);
    }
    s_btn_was_down = down;
}

static void shift_tick(lv_timer_t *t)
{
    (void)t;
    // Nudge everything a few pixels every couple of minutes so the AMOLED
    // doesn't burn in the static text. Offsets stay even for the panel.
    static const int8_t OFFS[][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {-2, 2}, {-2, 0}, {-2, -2}, {0, -2}, {2, -2}};
    s_shift = (s_shift + 1) % (int)(sizeof(OFFS) / sizeof(OFFS[0]));
    lv_obj_set_pos(s_root, OFFS[s_shift][0], OFFS[s_shift][1]);
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    crab_hop();
    if (!link_bridge_alive()) {
        return;
    }
    link_state_t st;
    link_get_state(&st);
    s_refreshing = true;
    s_refresh_started_us = esp_timer_get_time();
    s_refresh_fetched_at = st.fetched_at;
    link_request_refresh();
    ui_tick(NULL);
}

// ---------------------------------------------------------------- init

void ui_init(uint8_t brightness)
{
    s_brightness = brightness;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, NULL);

    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *crab = crab_create(s_root);
    lv_obj_align(crab, LV_ALIGN_TOP_MID, 0, 26);

    s_plan = make_label(s_root, &lv_font_montserrat_14, COL_MUTED);
    lv_obj_set_style_text_letter_space(s_plan, 3, 0);
    lv_label_set_text(s_plan, "CLAUDE CODE USAGE");
    lv_obj_align(s_plan, LV_ALIGN_TOP_MID, 0, 118);

    meter_create(&s_m5, "5-hour limit", 150);
    meter_create(&s_m7, "Weekly limit", 282);

    lv_obj_t *row = lv_obj_create(s_root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 414);

    s_dot = lv_obj_create(row);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_DIM), 0);

    s_ago = make_label(row, &lv_font_montserrat_16, COL_TEXT);

    s_detail = make_label(s_root, &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(s_detail, BOARD_LCD_H_RES - 2 * MARGIN);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 440);

    ui_tick(NULL);
    lv_timer_create(ui_tick, 250, NULL);
    lv_timer_create(button_tick, 30, NULL);
    lv_timer_create(shift_tick, 120 * 1000, NULL);
}
