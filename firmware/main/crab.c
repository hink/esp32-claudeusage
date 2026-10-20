// A little pixel crab in the style of Claude Code's mascot.
// Drawn into an RGB565 canvas at 8x scale so it stays crisp on the AMOLED.
//
// Sprite (18 x 10, '#' body, '.' empty):
//   ...############...
//   ...############...
//   ...##.######.##...   eyes
//   ...##.######.##...
//   .################.   arms
//   .################.
//   ...############...
//   ...############...
//   ....#.#....#.#....   legs
//   ....#.#....#.#....

#include "crab.h"

#include <string.h>

#define ART_W     18
#define ART_H     10
#define SCALE     8
#define CANVAS_W  (ART_W * SCALE)
#define CANVAS_H  (ART_H * SCALE)
#define WRAP_PAD  28

#define COL_BODY  0xD77757
#define COL_DROP  0x7CC8FF
#define COL_Z     0x8C8781

static uint16_t s_buf[CANVAS_W * CANVAS_H] __attribute__((aligned(4)));
static lv_obj_t *s_wrap;
static lv_obj_t *s_canvas;
static lv_obj_t *s_z;

static crab_mood_t s_mood = CRAB_NORMAL;
static bool s_mood_set;
static bool s_hopping;
static bool s_blink;
static int s_leg_phase;
static uint32_t s_next_blink;
static uint32_t s_blink_end;
static uint32_t s_next_leg;
static uint32_t s_last_key = UINT32_MAX;
static uint16_t s_body;
static uint16_t s_drop;

static void px(int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= ART_W || y >= ART_H) {
        return;
    }
    for (int yy = 0; yy < SCALE; yy++) {
        uint16_t *row = &s_buf[(y * SCALE + yy) * CANVAS_W + x * SCALE];
        for (int xx = 0; xx < SCALE; xx++) {
            row[xx] = c;
        }
    }
}

static void render(void)
{
    bool closed = s_mood == CRAB_SLEEP || s_blink;
    bool wide = s_mood == CRAB_PANIC && !closed;
    bool sweat = s_mood == CRAB_WORRIED || s_mood == CRAB_PANIC;
    bool arms_up = s_hopping;
    int legs = s_mood == CRAB_SLEEP ? 2 : s_leg_phase;

    uint32_t key = (closed << 0) | (wide << 1) | (sweat << 2) | (arms_up << 3) | ((uint32_t)legs << 4);
    if (key == s_last_key) {
        return;
    }
    s_last_key = key;

    memset(s_buf, 0, sizeof(s_buf));

    for (int y = 0; y < 8; y++) {
        for (int x = 3; x <= 14; x++) {
            px(x, y, s_body);
        }
    }

    // Eyes are holes in the body.
    if (closed) {
        px(5, 3, 0);
        px(12, 3, 0);
    } else if (wide) {
        for (int y = 2; y <= 3; y++) {
            px(5, y, 0);
            px(6, y, 0);
            px(11, y, 0);
            px(12, y, 0);
        }
    } else {
        for (int y = 2; y <= 3; y++) {
            px(5, y, 0);
            px(12, y, 0);
        }
    }

    if (arms_up) {
        for (int y = 2; y <= 3; y++) {
            px(1, y, s_body);
            px(2, y, s_body);
            px(15, y, s_body);
            px(16, y, s_body);
        }
        px(1, 1, s_body);
        px(16, 1, s_body);
    } else {
        for (int y = 4; y <= 5; y++) {
            px(1, y, s_body);
            px(2, y, s_body);
            px(15, y, s_body);
            px(16, y, s_body);
        }
    }

    // Legs: alternate pairs lift to look like walking; tucked when asleep.
    static const int cols[4] = {4, 6, 11, 13};
    for (int i = 0; i < 4; i++) {
        bool lifted = legs == 2 || ((i % 2) == legs);
        px(cols[i], 8, s_body);
        if (!lifted) {
            px(cols[i], 9, s_body);
        }
    }

    if (sweat && !arms_up) {
        px(16, 0, s_drop);
        px(16, 1, s_drop);
    }

    lv_obj_invalidate(s_canvas);
}

static void set_ty(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, v, 0);
}

static void set_tx(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x(obj, v, 0);
}

static void set_z(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, -v, 0);
    lv_obj_set_style_opa(obj, (lv_opa_t)(255 - (v * 255) / 30), 0);
}

static void loop_anim(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, bool playback)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    if (playback) {
        lv_anim_set_playback_duration(&a, ms);
    }
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void start_mood_anims(void)
{
    lv_anim_delete(s_wrap, NULL);
    lv_anim_delete(s_z, NULL);
    lv_obj_set_style_translate_x(s_wrap, 0, 0);
    lv_obj_set_style_translate_y(s_wrap, 0, 0);

    switch (s_mood) {
    case CRAB_SLEEP:
        loop_anim(s_wrap, set_ty, 0, -4, 2000, true);
        lv_obj_remove_flag(s_z, LV_OBJ_FLAG_HIDDEN);
        loop_anim(s_z, set_z, 0, 30, 1800, false);
        return;
    case CRAB_NORMAL:
        loop_anim(s_wrap, set_ty, 0, -6, 900, true);
        break;
    case CRAB_WORRIED:
        loop_anim(s_wrap, set_ty, 0, -6, 450, true);
        break;
    case CRAB_PANIC:
        loop_anim(s_wrap, set_tx, -6, 6, 90, true);
        break;
    }
    lv_obj_add_flag(s_z, LV_OBJ_FLAG_HIDDEN);
}

static void hop_done(lv_anim_t *a)
{
    (void)a;
    s_hopping = false;
    start_mood_anims();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();

    if (s_mood == CRAB_SLEEP) {
        s_blink = false;
    } else if (!s_blink && (int32_t)(now - s_next_blink) >= 0) {
        s_blink = true;
        s_blink_end = now + 140;
    } else if (s_blink && (int32_t)(now - s_blink_end) >= 0) {
        s_blink = false;
        s_next_blink = now + lv_rand(2500, 6000);
    }

    uint32_t leg_ms = s_mood == CRAB_NORMAL ? 450 : s_mood == CRAB_WORRIED ? 220 : s_mood == CRAB_PANIC ? 110 : 0;
    if (leg_ms && (int32_t)(now - s_next_leg) >= 0) {
        s_leg_phase ^= 1;
        s_next_leg = now + leg_ms;
    }
    render();
}

lv_obj_t *crab_create(lv_obj_t *parent)
{
    s_body = lv_color_to_u16(lv_color_hex(COL_BODY));
    s_drop = lv_color_to_u16(lv_color_hex(COL_DROP));

    s_wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(s_wrap);
    lv_obj_set_size(s_wrap, CANVAS_W + 2 * WRAP_PAD, CANVAS_H);
    lv_obj_add_flag(s_wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(s_wrap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_canvas = lv_canvas_create(s_wrap);
    lv_canvas_set_buffer(s_canvas, s_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, WRAP_PAD, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    s_z = lv_label_create(s_wrap);
    lv_label_set_text(s_z, "z");
    lv_obj_set_style_text_font(s_z, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_z, lv_color_hex(COL_Z), 0);
    lv_obj_set_pos(s_z, WRAP_PAD + CANVAS_W + 4, 0);
    lv_obj_add_flag(s_z, LV_OBJ_FLAG_HIDDEN);

    s_next_blink = lv_tick_get() + 3000;
    render();
    lv_timer_create(tick_cb, 50, NULL);
    crab_set_mood(CRAB_SLEEP);
    return s_wrap;
}

void crab_set_mood(crab_mood_t mood)
{
    if (s_mood_set && mood == s_mood) {
        return;
    }
    s_mood_set = true;
    s_mood = mood;
    if (!s_hopping) {
        start_mood_anims();
    }
    render();
}

void crab_hop(void)
{
    if (s_hopping) {
        return;
    }
    s_hopping = true;
    lv_anim_delete(s_wrap, NULL);
    lv_obj_set_style_translate_x(s_wrap, 0, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_wrap);
    lv_anim_set_exec_cb(&a, set_ty);
    lv_anim_set_values(&a, 0, -26);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_playback_duration(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, hop_done);
    lv_anim_start(&a);
    render();
}
