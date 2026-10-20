#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""Claude usage bridge.

Polls Anthropic's plan-usage endpoint (the one behind Claude Code's /usage)
with the login Claude Code already keeps in ~/.claude/.credentials.json, and
streams the numbers to the ESP32 display over USB serial.

The endpoint is undocumented and has been reported to lock clients out with
long-lived 429s when polled aggressively, so this polls every 5 minutes,
never more than once a minute (even on tap-to-refresh), and backs off hard
on 429. The token never leaves this machine; the bridge never refreshes it
(that would rotate Claude Code's refresh token), it just re-reads the file.
"""

from __future__ import annotations

import argparse
import glob
import json
import logging
import os
import re
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CREDS_PATH = Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home() / ".claude")) / ".credentials.json"
CACHE_PATH = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "claude-usage-bridge" / "state.json"
PORT_GLOB = "/dev/serial/by-id/usb-Espressif*"
ESPRESSIF_VID = 0x303A

MIN_GAP = 60          # hard floor between API calls, in seconds
HEARTBEAT = 10        # resend state to the board this often
RATE_LIMIT_BASE = 900 # first back-off after a 429
RATE_LIMIT_MAX = 3600
AUTH_RETRY = 1800     # retry after 401 even if the credentials file hasn't changed

log = logging.getLogger("claude-usage-bridge")


def iso_to_epoch(value) -> int:
    if not value:
        return 0
    try:
        return int(datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp())
    except ValueError:
        return 0


def tz_offset() -> int:
    return time.localtime().tm_gmtoff


class CredentialsError(Exception):
    pass


def _keychain_credentials() -> str | None:
    """macOS keeps Claude Code's login in the Keychain instead of a file."""
    if sys.platform != "darwin":
        return None
    try:
        out = subprocess.run(
            ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout.strip() if out.returncode == 0 and out.stdout.strip() else None


def read_credentials() -> dict:
    try:
        raw = CREDS_PATH.read_text()
    except FileNotFoundError as e:
        raw = _keychain_credentials()
        if raw is None:
            raise CredentialsError(f"{CREDS_PATH} not found - log in with `claude`") from e
    except OSError as e:
        raise CredentialsError(f"can't read {CREDS_PATH}: {e}") from e
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        raise CredentialsError(f"credentials are not valid JSON: {e}") from e
    oauth = data.get("claudeAiOauth") or {}
    if not oauth.get("accessToken"):
        raise CredentialsError("no Claude login in credentials file - log in with `claude`")
    return oauth


def plan_label(oauth: dict) -> str:
    tier = (oauth.get("rateLimitTier") or "").lower()
    m = re.search(r"max_(\d+)x", tier)
    if m:
        return f"Max {m.group(1)}x"
    sub = oauth.get("subscriptionType") or ""
    return sub.capitalize()


def creds_mtime() -> float:
    try:
        return CREDS_PATH.stat().st_mtime
    except OSError:
        return 0.0


def parse_usage(data: dict) -> tuple[dict | None, dict | None]:
    """Pull the 5-hour and weekly windows, tolerating either response shape."""

    def window(key: str, kind: str):
        w = data.get(key)
        if isinstance(w, dict) and w.get("utilization") is not None:
            return {"pct": float(w["utilization"]), "resets_at": iso_to_epoch(w.get("resets_at"))}
        for limit in data.get("limits") or []:
            if limit.get("kind") == kind and limit.get("percent") is not None:
                return {"pct": float(limit["percent"]), "resets_at": iso_to_epoch(limit.get("resets_at"))}
        return None

    return window("five_hour", "session"), window("seven_day", "weekly_all")


class Poller:
    def __init__(self, interval: int):
        self.interval = interval
        self.five_hour: dict | None = None
        self.seven_day: dict | None = None
        self.fetched_at = 0       # last successful fetch
        self.last_attempt = 0     # last API call of any outcome
        self.next_poll_at = 0
        self.status = "starting"
        self.detail = ""
        self.plan = ""
        self.rate_limit_strikes = 0
        self.offline_strikes = 0
        self.auth_mtime = 0.0     # credentials mtime when auth last failed
        self._load_cache()

    # -- persistence: keeps the rate-limit floor and last numbers across restarts
    def _load_cache(self):
        try:
            c = json.loads(CACHE_PATH.read_text())
        except (OSError, json.JSONDecodeError):
            self.next_poll_at = int(time.time())
            return
        self.five_hour = c.get("five_hour")
        self.seven_day = c.get("seven_day")
        self.fetched_at = int(c.get("fetched_at", 0))
        self.last_attempt = int(c.get("last_attempt", 0))
        self.status = c.get("status", "starting")
        self.detail = c.get("detail", "")
        self.plan = c.get("plan", "")
        self.rate_limit_strikes = int(c.get("rate_limit_strikes", 0))
        self.auth_mtime = float(c.get("auth_mtime", 0.0))
        now = int(time.time())
        saved_next = int(c.get("next_poll_at", 0))
        self.next_poll_at = max(saved_next, self.last_attempt + MIN_GAP) if saved_next else now
        if self.status == "ok":
            self.next_poll_at = min(self.next_poll_at, self.last_attempt + self.interval)
        log.info("loaded cached usage from %s (fetched %ds ago)", CACHE_PATH,
                 now - self.fetched_at if self.fetched_at else -1)

    def _save_cache(self):
        CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
        tmp = CACHE_PATH.with_suffix(".tmp")
        tmp.write_text(json.dumps({
            "five_hour": self.five_hour, "seven_day": self.seven_day,
            "fetched_at": self.fetched_at, "last_attempt": self.last_attempt,
            "next_poll_at": self.next_poll_at, "status": self.status, "detail": self.detail,
            "plan": self.plan, "rate_limit_strikes": self.rate_limit_strikes,
            "auth_mtime": self.auth_mtime,
        }))
        tmp.replace(CACHE_PATH)

    # -- scheduling
    def due(self, now: float) -> bool:
        if now - self.last_attempt < MIN_GAP:
            return False
        if self.status == "auth" and creds_mtime() != self.auth_mtime:
            return True  # Claude Code refreshed the login; try right away
        return now >= self.next_poll_at

    def can_refresh_now(self, now: float) -> tuple[bool, str]:
        if self.status == "rate_limited" and now < self.next_poll_at:
            return False, "Rate limited - can't refresh yet"
        wait = int(self.last_attempt + MIN_GAP - now)
        if wait > 0:
            return False, f"Just updated - try again in {wait}s"
        return True, ""

    def _schedule_ok(self, now: int):
        nxt = now + self.interval
        # Look again shortly after a window resets so the bar drops promptly.
        for w in (self.five_hour, self.seven_day):
            if w and w.get("resets_at", 0) > now:
                nxt = min(nxt, max(w["resets_at"] + 20, now + MIN_GAP))
        self.next_poll_at = nxt

    # -- the actual call
    def fetch(self):
        now = int(time.time())
        try:
            oauth = read_credentials()
        except CredentialsError as e:
            self.status, self.detail = "auth", str(e)
            self.auth_mtime = creds_mtime()
            self.next_poll_at = now + AUTH_RETRY
            log.warning("%s", e)
            self._save_cache()
            return

        self.plan = plan_label(oauth)
        expires = (oauth.get("expiresAt") or 0) / 1000
        if expires and expires < now + 30:
            # Don't spend a request on a token we know is dead. Claude Code
            # refreshes it next time it runs; we notice the file change.
            self.status, self.detail = "auth", "login token expired"
            self.auth_mtime = creds_mtime()
            self.next_poll_at = now + AUTH_RETRY
            log.warning("access token expired %ds ago; waiting for Claude Code to refresh it", now - expires)
            self._save_cache()
            return

        self.last_attempt = now
        req = urllib.request.Request(USAGE_URL, headers={
            "Authorization": f"Bearer {oauth['accessToken']}",
            "anthropic-beta": "oauth-2025-04-20",
            "Accept": "application/json",
            "User-Agent": "claude-usage-bridge/1.0",
        })
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                data = json.load(resp)
        except urllib.error.HTTPError as e:
            self._handle_http_error(e, now)
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            self.offline_strikes += 1
            self.status, self.detail = "offline", str(getattr(e, "reason", e))[:80]
            self.next_poll_at = now + min(self.interval, 60 * 2 ** (self.offline_strikes - 1))
            log.warning("network error: %s (retry in %ds)", self.detail, self.next_poll_at - now)
        except (json.JSONDecodeError, ValueError) as e:
            self.status, self.detail = "error", "bad response from usage endpoint"
            self.next_poll_at = now + self.interval
            log.warning("bad JSON from endpoint: %s", e)
        else:
            five, seven = parse_usage(data)
            if five is None and seven is None:
                self.status, self.detail = "error", "usage response format changed"
                self.next_poll_at = now + self.interval
                log.warning("no usage windows in response; keys: %s", sorted(data)[:12])
            else:
                self.five_hour, self.seven_day = five, seven
                self.fetched_at = now
                self.status, self.detail = "ok", ""
                self.rate_limit_strikes = 0
                self.offline_strikes = 0
                self._schedule_ok(now)
                log.info("5h %s%% | week %s%% | next poll in %ds",
                         five and round(five["pct"], 1), seven and round(seven["pct"], 1),
                         self.next_poll_at - now)
        self._save_cache()

    def _handle_http_error(self, e: urllib.error.HTTPError, now: int):
        self.offline_strikes = 0
        if e.code == 429:
            self.rate_limit_strikes += 1
            try:
                retry_after = int(e.headers.get("retry-after") or 0)
            except ValueError:
                retry_after = 0
            backoff = min(RATE_LIMIT_MAX, RATE_LIMIT_BASE * 2 ** (self.rate_limit_strikes - 1))
            self.next_poll_at = now + max(retry_after, backoff)
            self.status, self.detail = "rate_limited", "HTTP 429"
            log.warning("rate limited (429); backing off %ds", self.next_poll_at - now)
        elif e.code in (401, 403):
            self.status, self.detail = "auth", f"HTTP {e.code}"
            self.auth_mtime = creds_mtime()
            self.next_poll_at = now + AUTH_RETRY
            log.warning("auth rejected (%d); waiting for Claude Code to refresh login", e.code)
        else:
            self.status, self.detail = "error", f"Usage endpoint HTTP {e.code}"
            self.next_poll_at = now + self.interval
            log.warning("HTTP %d from usage endpoint", e.code)

    def state_message(self, notice: str = "") -> dict:
        return {
            "type": "state",
            "now": int(time.time()),
            "tz": tz_offset(),
            "plan": self.plan,
            "status": self.status,
            "detail": self.detail,
            "notice": notice,
            "fetched_at": self.fetched_at,
            "next_poll_at": int(self.next_poll_at),
            "five_hour": self.five_hour,
            "seven_day": self.seven_day,
        }


class DeviceLink:
    def __init__(self, port: str | None):
        self.port_arg = port
        self.ser: serial.Serial | None = None
        self.buf = b""
        self.next_try = 0.0

    def _find_port(self) -> str | None:
        if self.port_arg:
            return self.port_arg
        ports = sorted(glob.glob(PORT_GLOB))  # stable names on Linux
        if ports:
            return ports[0]
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID:
                return p.device
        return None

    def connected(self) -> bool:
        return self.ser is not None

    def try_connect(self) -> bool:
        if self.ser or time.monotonic() < self.next_try:
            return self.ser is not None
        self.next_try = time.monotonic() + 3
        path = self._find_port()
        if not path:
            return False
        try:
            # Default DTR/RTS (both high) does not reset the ESP32-C6's USB
            # Serial/JTAG; RTS high with DTR low would.
            self.ser = serial.Serial(path, 115200, timeout=0.5, write_timeout=2)
            self.ser.reset_input_buffer()
            self.buf = b""
            log.info("connected to display on %s", path)
            return True
        except (serial.SerialException, OSError) as e:
            log.debug("can't open %s: %s", path, e)
            self.ser = None
            return False

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def send(self, msg: dict) -> bool:
        if not self.ser:
            return False
        try:
            self.ser.write((json.dumps(msg, separators=(",", ":")) + "\n").encode())
            return True
        except (serial.SerialException, OSError) as e:
            log.warning("display write failed: %s", e)
            self.close()
            return False

    def read_messages(self) -> list[dict]:
        if not self.ser:
            time.sleep(0.5)
            return []
        try:
            self.buf += self.ser.read(4096)
        except (serial.SerialException, OSError) as e:
            log.warning("display disconnected: %s", e)
            self.close()
            return []
        msgs = []
        *lines, self.buf = self.buf.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").strip()
            if line.startswith("@{"):
                try:
                    msgs.append(json.loads(line[1:]))
                except json.JSONDecodeError:
                    pass
            elif line:
                log.debug("board: %s", line)
        return msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial port (default: first Espressif USB device)")
    ap.add_argument("--interval", type=int, default=300, help="seconds between polls (default 300, minimum 120)")
    ap.add_argument("--brightness", type=int, help="set display brightness 0-100 on connect")
    ap.add_argument("--once", action="store_true", help="fetch once, print the result, and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")

    if args.interval < 120:
        log.warning("interval %ds is too aggressive for this endpoint; using 120s", args.interval)
        args.interval = 120

    poller = Poller(args.interval)

    if args.once:
        if time.time() - poller.last_attempt < MIN_GAP:
            log.info("last call was under %ds ago; showing cached data", MIN_GAP)
        else:
            poller.fetch()
        print(json.dumps(poller.state_message(), indent=2))
        return

    link = DeviceLink(args.port)
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    last_sent = 0.0
    last_hello = 0.0
    log.info("polling every %ds; cache at %s", args.interval, CACHE_PATH)

    while running:
        newly_connected = not link.connected() and link.try_connect()
        notice = ""
        push = newly_connected

        for msg in link.read_messages():
            kind = msg.get("type")
            if kind == "hello":
                # Hellos queue up while nobody has the port open; treat a burst as one.
                if time.monotonic() - last_hello > 4:
                    log.info("display says hello (fw %s)", msg.get("fw", "?"))
                    push = True
                    newly_connected = True
                last_hello = time.monotonic()
            elif kind == "refresh":
                ok, why = poller.can_refresh_now(time.time())
                if ok:
                    log.info("refresh requested from display")
                    poller.fetch()
                else:
                    notice = why
                push = True

        if newly_connected and args.brightness is not None:
            link.send({"type": "config", "brightness": max(0, min(100, args.brightness))})

        if poller.due(time.time()):
            poller.fetch()
            push = True

        if push or time.monotonic() - last_sent >= HEARTBEAT:
            if link.send(poller.state_message(notice)):
                last_sent = time.monotonic()

    link.close()
    log.info("stopped")


if __name__ == "__main__":
    main()
