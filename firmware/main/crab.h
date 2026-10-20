#pragma once

#include "lvgl.h"

typedef enum {
    CRAB_SLEEP,    // no data / PC bridge gone
    CRAB_NORMAL,   // usage comfortable
    CRAB_WORRIED,  // a limit is >= 75%
    CRAB_PANIC,    // a limit is >= 95%
} crab_mood_t;

// Creates the crab (about 200x80 px). Call with the LVGL lock held.
lv_obj_t *crab_create(lv_obj_t *parent);
void crab_set_mood(crab_mood_t mood);
void crab_hop(void);
