#pragma once

// USB serial link to the PC bridge.
//
// PC -> board: one JSON object per line, e.g.
//   {"type":"state","now":1792500000,"tz":-18000,"plan":"Max 20x",
//    "status":"ok","detail":"","notice":"","fetched_at":1792499900,"next_poll_at":1792500200,
//    "five_hour":{"pct":12.0,"resets_at":1792510000},
//    "seven_day":{"pct":6.0,"resets_at":1792700000}}
//   {"type":"config","brightness":70}
//   {"type":"notice","text":"Hold upright","secs":5}      footer message
//   {"type":"imu"}                                        one accelerometer reading
//   {"type":"imu_stream","secs":30}                       readings at 10 Hz
//   {"type":"orient_cal","a0":1,"d":1,"flip":0}           save auto-rotate calibration
//
// Board -> PC: lines starting with '@' followed by JSON, e.g.
//   @{"type":"hello","fw":"1.0.0"}
//   @{"type":"refresh"}
//   @{"type":"imu","x":..,"y":..,"z":..,"up":1,"rot":0,"cal":[a0,d,flip]}

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;         // window present in the last update
    float pct;          // 0..100
    int64_t resets_at;  // unix seconds, 0 if unknown
} usage_window_t;

typedef struct {
    bool have_time;      // a clock sync has been received
    bool have_usage;     // at least one successful fetch has been received
    usage_window_t five_hour;
    usage_window_t seven_day;
    int64_t fetched_at;  // unix seconds of the last successful fetch
    int64_t next_poll_at;
    int32_t tz_offset;   // seconds east of UTC
    char status[16];     // ok | rate_limited | auth | offline | error | starting
    char detail[96];
    char notice[64];
    int64_t notice_until_us;
    char plan[24];
    int64_t last_rx_us;  // esp_timer time of last message from the PC
    uint32_t version;    // bumps on every change
} link_state_t;

void link_init(void);
void link_get_state(link_state_t *out);

// Current unix time based on the last sync from the PC, or 0 if never synced.
int64_t link_now(void);

// True if the PC bridge has sent anything recently.
bool link_bridge_alive(void);

// Ask the PC for an immediate refresh (it may decline to protect the rate limit).
void link_request_refresh(void);

// Brightness pushed by the PC, or -1 if none yet. Cleared after reading.
int link_take_brightness_request(void);
