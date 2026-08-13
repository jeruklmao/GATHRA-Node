#!/usr/bin/env python3
"""Bounded reconnecting serial capture for deep-sleep HIL runs."""

import argparse
import os
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    deadline = time.monotonic() + args.duration
    connection = None
    while time.monotonic() < deadline:
        if connection is None:
            if not os.path.exists(args.port):
                time.sleep(0.05)
                continue
            try:
                connection = serial.Serial(args.port, args.baud, timeout=0.1)
                print(f"[capture] opened {args.port}", flush=True)
                if args.reset:
                    connection.rts = True
                    time.sleep(0.2)
                    connection.rts = False
                    time.sleep(0.3)
                    args.reset = False
            except (OSError, serial.SerialException):
                connection = None
                time.sleep(0.05)
                continue
        try:
            data = connection.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
        except (OSError, serial.SerialException) as error:
            print(f"\n[capture] disconnected: {error}", flush=True)
            try:
                connection.close()
            except (OSError, serial.SerialException):
                pass
            connection = None
    if connection is not None:
        connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
