#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""One-time auto-rotate calibration for the Claude usage display.

The accelerometer's axes are fixed to the board, but which way is "up" for the
screen depends on how the sensor is mounted. This walks you through two poses
using prompts on the display itself (tap the screen to confirm each):

  1. stand it upright so the text reads correctly, tap
  2. turn it a quarter turn clockwise, tap

The result is saved on the board. Pauses the bridge service while it runs.

  uv run tools/calibrate_orientation.py           # calibrate
  uv run tools/calibrate_orientation.py --flip    # toggle 90/270 if sideways comes out upside down
  uv run tools/calibrate_orientation.py --show    # print a reading
  uv run tools/calibrate_orientation.py --clear   # disable auto-rotate
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

SERVICE = "claude-usage-bridge"
AXES = ["+X", "+Y", "-X", "-Y"]


class Board:
    def __init__(self, port: str):
        self.ser = serial.Serial(port, 115200, timeout=0.1, write_timeout=2)
        self.buf = b""

    def send(self, msg: dict):
        self.ser.write((json.dumps(msg) + "\n").encode())

    def notice(self, text: str, secs: int = 90):
        self.send({"type": "notice", "text": text, "secs": secs})

    def messages(self):
        self.buf += self.ser.read(4096)
        *lines, self.buf = self.buf.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").strip()
            if line.startswith("@{"):
                try:
                    yield json.loads(line[1:])
                except json.JSONDecodeError:
                    pass

    def one_reading(self, timeout=3.0) -> dict | None:
        self.send({"type": "imu"})
        end = time.time() + timeout
        while time.time() < end:
            for m in self.messages():
                if m.get("type") == "imu":
                    return m
        return None


def wait_for_tap(board: Board, prompt: str, history: collections.deque) -> int | None:
    """Stream readings until the screen is tapped; return the steady up-axis before the tap."""
    board.notice(prompt)
    print(f"  -> {prompt}")
    last_stream = 0.0
    deadline = time.time() + 180
    while time.time() < deadline:
        if time.time() - last_stream > 20:
            board.send({"type": "imu_stream", "secs": 30})
            board.notice(prompt)
            last_stream = time.time()
        for m in board.messages():
            if m.get("type") == "imu" and "up" in m:
                history.append(m)
            elif m.get("type") == "refresh":
                # Skip the last ~0.3 s: pressing the screen jolts the board.
                recent = [h["up"] for h in list(history)[-13:-3]]
                if len(recent) >= 5:
                    up, n = collections.Counter(recent).most_common(1)[0]
                    if up >= 0 and n >= 0.8 * len(recent):
                        v = history[-1]
                        print(f"     tap: up axis {AXES[up]}  (x={v['x']} y={v['y']} z={v['z']})")
                        return up
                board.notice("Couldn't tell which way is up - hold steady and tap again")
                print("     tap, but the board wasn't steady/upright; waiting for another tap")
                time.sleep(2)
                board.notice(prompt)
    return None


def main():
    ap = argparse.ArgumentParser(description="Calibrate auto-rotate on the Claude usage display")
    ap.add_argument("--port")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--flip", action="store_true", help="toggle the 90/270 direction of an existing calibration")
    g.add_argument("--show", action="store_true", help="print one accelerometer reading")
    g.add_argument("--clear", action="store_true", help="remove calibration (no auto-rotate)")
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    port = (args.port
            or next(iter(sorted(glob.glob("/dev/serial/by-id/usb-Espressif*"))), None)
            or next((p.device for p in list_ports.comports() if p.vid == 0x303A), None))
    if not port:
        sys.exit("No Espressif board found on USB")

    was_active = subprocess.run(["systemctl", "--user", "is-active", "--quiet", SERVICE]).returncode == 0
    if was_active:
        subprocess.run(["systemctl", "--user", "stop", SERVICE], check=True)
    try:
        board = Board(port)
        time.sleep(0.3)
        r = board.one_reading()
        if r is None:
            sys.exit("Board didn't answer; is the 1.1+ firmware flashed?")
        if "error" in r:
            sys.exit(f"Board says: {r['error']}")
        a0, d, flip = r["cal"]

        if args.show:
            print(json.dumps(r))
            return
        if args.clear:
            board.send({"type": "orient_cal", "a0": -1, "d": 1, "flip": 0})
            print("Calibration cleared; display stays at its default rotation.")
            return
        if args.flip:
            if a0 < 0:
                sys.exit("Not calibrated yet; run without --flip first")
            board.send({"type": "orient_cal", "a0": a0, "d": d, "flip": 0 if flip else 1})
            board.notice("Rotation direction flipped", 6)
            print(f"flip is now {0 if flip else 1}")
            return

        print("Calibrating auto-rotate. Follow the prompts on the display.")
        history: collections.deque = collections.deque(maxlen=50)
        up0 = wait_for_tap(board, "Calibrating: stand me upright so this reads right, then tap me", history)
        if up0 is None:
            sys.exit("Timed out")
        time.sleep(1.0)
        while True:
            up1 = wait_for_tap(board, "Now turn me a quarter turn clockwise and tap me", history)
            if up1 is None:
                sys.exit("Timed out")
            step = (up1 - up0) % 4
            if step in (1, 3):
                break
            board.notice("That's not a quarter turn - try again", 4)
            print("     that wasn't a quarter turn; asking again")
            time.sleep(3)

        board.send({"type": "orient_cal", "a0": up0, "d": step, "flip": 0})
        board.notice("Done! It should now read correctly on any side", 8)
        print(f"Saved: upright = {AXES[up0]}, clockwise step = {step}. Turn it around to check.")
        print("If sideways comes out upside down, run with --flip.")
        time.sleep(1)
    finally:
        if was_active:
            subprocess.run(["systemctl", "--user", "start", SERVICE])


if __name__ == "__main__":
    main()
