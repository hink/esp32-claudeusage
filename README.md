# Claude Usage Display

A desk display that shows your **Claude plan limits** in real time on a
[Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm):
one bar for the **5-hour window** and one for the **weekly limit**, with reset countdowns,
how long ago the numbers were fetched, and a small pixel crab that reacts to your usage.

```
┌────────────────────────────────────────┐
│                ▐▛███▜▌                 │
│               ▝▜█████▛▘                │
│                 ▘▘ ▝▝                  │
│            CLAUDE MAX 20X              │
│                                        │
│  5-hour limit                     42%  │
│  ████████████████░░░░░░░░░░░░░░░░░░░░  │
│  Resets in 2h 14m  (3:40 PM)           │
│                                        │
│  Weekly limit                      6%  │
│  ███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │
│  Resets in 3d 1h  (Thu 11:00 AM)       │
│                                        │
│           ● Updated 2m ago             │
│   Next update in 3m - tap to refresh   │
└────────────────────────────────────────┘
```

> Unofficial fan project, not affiliated with or endorsed by Anthropic. It reads
> an **undocumented** endpoint that could change or disappear at any time, and
> scripted use of a subscription login is outside Anthropic's terms. Read
> [Terms of service](#terms-of-service) before you use it.

## Features

- **Live plan limits:** the same numbers Claude Code's `/usage` shows, for both the 5-hour session window and the weekly limit.
- **Reset countdowns** in relative and local time ("Resets in 2h 14m (3:40 PM)").
- **Freshness indicator:** "Updated 2m ago" with a dot that's green while the bridge is on schedule and turns amber, then red, when a poll is overdue.
- **Pixel crab** in the style of Claude Code's mascot. It walks, blinks, sweats at 75%, panics at 95%, and falls asleep when the PC disconnects.
- **Tap to refresh**, limited to once a minute.
- **Auto-rotate** using the on-board accelerometer, so it reads correctly on any edge.
- **BOOT button** cycles brightness (100 / 70 / 40 / 15%). The setting is remembered.
- **Burn-in protection:** the whole UI shifts a few pixels every 2 minutes.
- **Smart polling:** every 5 minutes while you're using Claude Code, hourly when you're not, and once after each limit resets. It backs off hard on 429s, keeps its schedule across restarts, and makes no requests while the display is unplugged.
- **Your token never leaves your computer.** The board has no network access and holds no credentials.

## How it works

```
┌──────────────── your computer ─────────────────┐            ┌──── ESP32-C6 ────┐
│                                                │            │                  │
│  Claude Code ──writes──▶ ~/.claude/            │            │  LVGL UI         │
│                          .credentials.json     │            │  crab, bars,     │
│                                │ reads token   │  USB-C     │  countdowns      │
│                                ▼               │  serial    │                  │
│  claude_usage_bridge.py ─────────────────────────JSON lines─▶  link.c          │
│        │ 5 min active / 60 min idle            │  (power +  │                  │
│        ▼                                       │   data)    │  accelerometer   │
│  api.anthropic.com/api/oauth/usage             │            │  ──▶ rotation    │
└────────────────────────────────────────────────┘            └──────────────────┘
```

A small Python **bridge** runs on your computer. It reads the OAuth token Claude Code
already keeps, calls the usage endpoint (often while you're using Claude Code, rarely
when you're not), and streams the results over the USB cable as JSON lines. The
**firmware** only draws them. The board gets the time from the same messages, so
countdowns and "Updated X ago" keep ticking between polls and even if the PC goes away.

Why USB instead of Wi-Fi? Claude Code refreshes its OAuth token on your computer. If
the board fetched the data itself, it would need its own copy of the token, and
refreshing that copy would rotate the refresh token and sign Claude Code out. With USB
there's also no Wi-Fi setup and no secrets on the device.

## Requirements

| | |
|---|---|
| Display | Waveshare **ESP32-C6-Touch-AMOLED-2.16** (480×480 AMOLED, CST9217 touch, QMI8658 IMU, AXP2101 PMIC) |
| Claude | Claude Code logged in with a **Pro or Max** subscription (`claude` → `/login`). API-key users have no plan limits to show. |
| Computer | Linux with systemd (tested on Fedora 44). macOS should work for the bridge, but it's untested. See [macOS](#macos). |
| Tools | [`uv`](https://docs.astral.sh/uv/) for the Python scripts and [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c6/get-started/) to build the firmware |

## Quick start

### 1. (Optional) Back up the factory firmware

```sh
uvx --from esptool esptool -p /dev/ttyACM0 -b 921600 read-flash 0 0x1000000 factory-backup.bin
```

Keep this file private. The factory app stores Wi-Fi credentials in flash if you ever set them.

### 2. Build and flash the firmware

```sh
./flash.sh
```

`flash.sh` builds with ESP-IDF and flashes with esptool. It uses `idf.py` from your
PATH, or sources `IDF_EXPORT` (default `~/esp/esp-idf-v5.5.3/export.sh`). To build
inside a distrobox or toolbox, set `DISTROBOX=<name>`. Put local settings in
`flash.env`, which is git-ignored:

```sh
echo 'DISTROBOX=dev' > flash.env
```

Or build by hand:

```sh
cd firmware
idf.py set-target esp32c6   # first time only
idf.py build flash
```

### 3. Start the bridge

As a systemd user service, which starts at login and restarts on failure:

```sh
./bridge/install-service.sh            # extra args go to the bridge, e.g. --brightness 40
journalctl --user -u claude-usage-bridge -f
```

Or run it in a terminal:

```sh
uv run bridge/claude_usage_bridge.py -v
```

Within a few seconds the crab wakes up and your numbers appear.

### 4. Calibrate auto-rotate (once)

The accelerometer is fixed to the board, but its axes relative to the screen have to
be measured once:

```sh
uv run tools/calibrate_orientation.py
```

Follow the prompts at the bottom of the display:

1. Stand it upright so the text reads correctly, then tap the screen.
2. Turn it a quarter turn clockwise, then tap again.

The calibration is stored on the board and survives reboots and reflashing. If
sideways orientations come out upside down, run it again with `--flip`.

## Using it

| | |
|---|---|
| **Tap the screen** | Refresh now. The bridge allows one API call per minute, so it may answer "Just updated - try again in 40s". |
| **BOOT button** | Cycle brightness: 100 → 70 → 40 → 15%. |
| **Turn it** | The UI rotates once the board has been steady for about 0.5 s. Lying flat keeps the last orientation. |

**Bar colors:** orange below 75%, amber from 75%, red from 90%.

**Status dot and footer:**

| Footer | Meaning |
|---|---|
| ● green "Updated 2m ago" | The bridge is on schedule. During idle periods that can mean data up to an hour old, which is fine because usage only changes while you use Claude. |
| ● amber | A scheduled poll is 2–30 min overdue (it failed, is rate limited, or the bridge is gone) |
| ● red | A poll is more than 30 min overdue, or an error |
| "Rate limited - next try in 14m" | The endpoint returned 429; the bridge is backing off |
| "Login expired - open Claude Code" | The OAuth token expired. Run `claude` once and it recovers by itself. |
| "Can't reach Anthropic - retrying" | Network problem on the computer |
| "PC bridge disconnected" | No message from the bridge for 45 s. The crab goes to sleep. |
| "Limit reset - updating soon" | A window passed its reset time; the bridge polls shortly after |

**Crab moods:** walks normally, sweats at 75% or more on either limit, panics at 95%
or more, and sleeps when the bridge is gone.

## The bridge

```
uv run bridge/claude_usage_bridge.py [--interval 300] [--idle-interval 3600] [--active-window 600]
                                     [--brightness N] [--port PATH] [--once] [-v]
```

| Option | |
|---|---|
| `--interval` | Seconds between polls while Claude Code is active. Default 300; anything under 120 is raised to 120. |
| `--idle-interval` | Seconds between polls while it's idle. Default 3600; anything under 600 is raised to 600. |
| `--active-window` | How long after the last Claude Code activity still counts as active. Default 600 seconds. |
| `--brightness` | Set the display brightness (0–100) whenever the board connects. Without it, the BOOT-button setting is kept. |
| `--port` | Serial port. By default it uses the first Espressif USB device. |
| `--once` | Fetch once (unless the last call was under a minute ago), print the result, and exit. Handy for testing. |
| `-v` | Debug logging, including the board's own log lines. |

### Smart polling

Your usage only changes while you're using Claude, so the bridge polls based on activity.
It detects Claude Code activity locally from file timestamps: it looks at
`~/.claude/projects/*/*.jsonl` and `~/.claude/history.jsonl` every 15 seconds and reads
nothing from them.

| Situation | Poll |
|---|---|
| Claude Code wrote a transcript in the last 10 min | every 5 min |
| Idle, but there was activity since the last poll | once, straight away (catch-up) |
| Idle | every 60 min, which also catches claude.ai or desktop-app use |
| A 5-hour or weekly window just passed its reset time | once, 20 s after the reset |
| Display unplugged | never; it polls once when you plug it back in, if a poll came due |

A typical day comes to a few dozen requests instead of 288 at a flat 5 minutes.
"Updated X ago" and the countdowns keep ticking on the board between polls. Tap the
screen to force a refresh any time.

### Rate limits

The endpoint isn't documented and doesn't send rate-limit headers. Third-party tools
that poll it aggressively have reported **persistent 429s lasting 30+ minutes**
([anthropics/claude-code#30930](https://github.com/anthropics/claude-code/issues/30930),
[#31637](https://github.com/anthropics/claude-code/issues/31637)). The bridge is
deliberately conservative:

- It polls at most every **5 minutes**, and much less when you're idle (see above).
- There's a hard floor of **60 seconds** between calls, which also applies to tap-to-refresh.
- On **429** it backs off 15 → 30 → 60 minutes and honors `Retry-After`.
- Its schedule and last result are saved to `~/.cache/claude-usage-bridge/state.json`, so restarting the bridge or replugging the board never triggers an extra call. The board gets the cached numbers immediately.

### Token handling

- The bridge **only reads** `~/.claude/.credentials.json` (or `$CLAUDE_CONFIG_DIR`). It re-reads the file before every poll, so it always uses the token Claude Code most recently refreshed.
- It **never refreshes the token itself**. OAuth refresh tokens rotate, so refreshing from a second program would sign Claude Code out.
- If the token has expired, which happens when Claude Code hasn't run for several hours, the bridge skips the request and shows "Login expired". As soon as Claude Code rewrites the credentials file, the bridge notices and fetches straight away.
- The token is only sent to `api.anthropic.com`. It is never logged and never sent to the board.

### macOS

Claude Code on macOS keeps its login in the Keychain. The bridge falls back to
`security find-generic-password -s "Claude Code-credentials" -w` when the credentials
file doesn't exist, and finds the board by its USB vendor ID. Both are **untested**.
There's no launchd service file; run the bridge in a terminal or write a LaunchAgent.

### Response format

The bridge accepts both shapes the endpoint has used: `five_hour` / `seven_day`
objects with `utilization` and `resets_at`, and the newer `limits` array
(`kind: "session"` / `"weekly_all"`). If neither is present, the display shows
"usage response format changed".

## Serial protocol

The connection is newline-delimited JSON over the ESP32-C6's built-in USB Serial/JTAG
port. Opening the port with the default DTR/RTS doesn't reset the board.

**Computer → board**

| Message | Purpose |
|---|---|
| `{"type":"state","now":…,"tz":…,"plan":"Max 20x","status":"ok","detail":"","notice":"","fetched_at":…,"next_poll_at":…,"five_hour":{"pct":42.0,"resets_at":…},"seven_day":{…}}` | Full state. Sent after each poll and every 10 s as a heartbeat. Times are Unix seconds; `tz` is the UTC offset in seconds. |
| `{"type":"config","brightness":70}` | Set brightness |
| `{"type":"notice","text":"…","secs":5}` | Show a temporary footer message |
| `{"type":"imu"}` / `{"type":"imu_stream","secs":30}` | One accelerometer reading, or a 10 Hz stream |
| `{"type":"orient_cal","a0":1,"d":3,"flip":0}` | Store the auto-rotate calibration (`a0:-1` clears it) |

**Board → computer.** These lines are prefixed with `@` so they stand apart from ESP-IDF log output:

| Message | Purpose |
|---|---|
| `@{"type":"hello","fw":"1.2.0"}` | Sent every 5 s until the computer talks. The bridge replies with the cached state. |
| `@{"type":"refresh"}` | The screen was tapped |
| `@{"type":"imu","x":…,"y":…,"z":…,"up":1,"rot":0,"cal":[1,3,0]}` | Accelerometer reading |

## Firmware

ESP-IDF v5.5.3, LVGL 9.2, esp_lvgl_port 2.6. The app is about 700 KB. Wi-Fi and
Bluetooth are never started.

| File | |
|---|---|
| `main/board.c` | Pin map, AXP2101 power rails, QSPI AMOLED panel, CST9217 touch, LVGL port |
| `main/link.c` | USB serial protocol, clock sync, shared state |
| `main/ui.c` | Layout, meters, footer, tap, BOOT button, burn-in shift |
| `main/crab.c` | The 18×10 pixel crab, drawn at 8× on a canvas, with moods and animations |
| `main/imu.c` | Minimal QMI8658 accelerometer driver |
| `main/orient.c` | Orientation detection, debouncing and calibration (stored in NVS) |

### Pin map

| Function | GPIO | | I2C device | Address |
|---|---|---|---|---|
| QSPI CLK | 0 | | AXP2101 PMIC | 0x34 |
| QSPI D0–D3 | 1, 2, 3, 4 | | CST9217 touch | 0x5A |
| LCD CS | **15** | | QMI8658 IMU | 0x6B |
| Touch INT / RST | **5** / 11 | | | |
| I2C SDA / SCL | 8 / 7 | | | |
| BOOT button | 9 | | | |

### Hardware notes

- **CS and touch INT pins:** the `user_config.h` in Waveshare's ESP-IDF examples lists LCD CS = 5 and touch INT = 15. Those are swapped. The working values (CS 15, INT 5) match Waveshare's Arduino demo and the XiaoZhi board config.
- **Panel:** a CO5300 driven with the `esp_lcd_sh8601` driver and Waveshare's init sequence. It has no reset pin, so it's power-cycled through the PMIC's ALDO3. Brightness is command `0x51`. Flush areas must start on even and end on odd coordinates (handled by a rounder callback).
- **No hardware rotation:** the panel can't swap X/Y, so rotation is done in software by esp_lvgl_port. That costs one extra 38 KB buffer. LVGL rotates touch input to match.
- **IMU configuration:** the QMI8658 stays powered across ESP32 resets and ignores range changes while it's running. The driver turns the sensors off before configuring them; otherwise you inherit whatever range the previous firmware set.
- **Brightness at boot:** the panel starts at brightness 0 and lights up after the first frame, so random GRAM contents never flash on screen.

## Troubleshooting

| Problem | Fix |
|---|---|
| Screen says "Waiting for PC bridge" | Check `systemctl --user status claude-usage-bridge` and `journalctl --user -u claude-usage-bridge`. |
| `Permission denied: /dev/ttyACM0` | Add yourself to the serial group (`sudo usermod -aG dialout $USER` on most distros, `uucp` on Arch), then log out and back in. |
| Flashing fails with "port is busy" | Something else has the port open. `flash.sh` stops the bridge for you; stop any serial monitors too. |
| "Login expired - open Claude Code" | Run `claude` once. The bridge picks up the refreshed token within a minute. |
| "Rate limited" | Wait. The bridge is already backing off. Don't lower `--interval`. |
| Sideways orientations are upside down | `uv run tools/calibrate_orientation.py --flip` |
| Calibration says "Couldn't tell which way is up" | Hold the board still with one edge clearly down, not lying flat, then tap. |
| Want the factory demo back | `uvx --from esptool esptool -p /dev/ttyACM0 -b 921600 write-flash 0 factory-backup.bin` |

## Terms of service

**Use this at your own risk.**

- **Consumer Terms:** Anthropic's [Consumer Terms](https://www.anthropic.com/legal/consumer-terms), which cover Free, Pro and Max, prohibit accessing the services "through automated or non-human means, whether through a bot, script, or otherwise" unless you use an API key.
- **Claude Code's legal page:** Claude Code's [legal and compliance page](https://code.claude.com/docs/en/legal-and-compliance) says subscription OAuth "is designed to support ordinary use of Claude Code and other native Anthropic applications." Anthropic may enforce this "without prior notice."

A script that polls the usage endpoint with your subscription login falls outside that.

As of October 2026, enforcement has targeted third-party tools that use subscription
logins to *run the model*, often by impersonating Claude Code. We're not aware of
accounts being suspended for reading their own usage numbers, and several community
tools poll the same endpoint. That isn't a guarantee.

This project keeps its footprint small:
- it only reads, and only your own account;
- it never makes model requests;
- it makes a few dozen calls on a typical day;
- it sends an honest `User-Agent` (`claude-usage-bridge/1.0`) instead of impersonating Claude Code.

There is a supported alternative with no terms question: Claude Code passes the same
5-hour and weekly percentages to your [status line](https://code.claude.com/docs/en/statusline)
script (`rate_limits.five_hour` / `rate_limits.seven_day`) after every response. This
project doesn't use that source yet, but it would be a drop-in replacement for the
endpoint.

## Credits

- Board support details come from Waveshare's [ESP32-C6-Touch-AMOLED-2.16 examples](https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16) and the XiaoZhi board configuration.
- Built with [ESP-IDF](https://github.com/espressif/esp-idf), [LVGL](https://lvgl.io), [esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port) and [cJSON](https://github.com/DaveGamble/cJSON).
- The crab is inspired by Claude Code's welcome-screen mascot.

## License

[MIT](LICENSE)
