#!/usr/bin/env python3
"""CLI for the minimal WireOS Vector hardware API."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from vector_robot import VectorRobot


def print_json(value: object) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def main() -> int:
    p = argparse.ArgumentParser(prog="vectorctl")
    p.add_argument("--host", default="192.168.1.89")
    p.add_argument("--port", type=int, default=8080)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("sensors")
    sub.add_parser("motors")

    move = sub.add_parser("move")
    move.add_argument("motor", type=int, choices=range(4))
    move.add_argument("ticks", type=int)
    move.add_argument("--power", type=float, default=0.5)
    drive = sub.add_parser("drive")
    drive.add_argument("ticks", type=int)
    drive.add_argument("--power", type=float, default=0.35)
    drive.add_argument("--timeout-ms", type=int, default=10000)

    hold = sub.add_parser("hold")
    hold.add_argument("motor", type=int, choices=range(4))
    hold.add_argument("--target", type=int)
    hold.add_argument("--power", type=float, default=0.7)
    hold.add_argument("--deadband", type=int, default=6)

    release = sub.add_parser("release")
    release.add_argument("motor", type=int, choices=range(4))

    raw = sub.add_parser("raw")
    raw.add_argument("--left", type=float, default=0)
    raw.add_argument("--right", type=float, default=0)
    raw.add_argument("--lift", type=float, default=0)
    raw.add_argument("--head", type=float, default=0)
    raw.add_argument("--ttl-ms", type=int, default=250)

    sub.add_parser("stop")

    leds = sub.add_parser("leds")
    leds.add_argument("r", type=int)
    leds.add_argument("g", type=int)
    leds.add_argument("b", type=int)

    snap = sub.add_parser("snapshot")
    snap.add_argument("out", nargs="?", default="vector-snapshot.bmp")

    args = p.parse_args()
    robot = VectorRobot(args.host, args.port)

    if args.cmd == "status":
        print_json(robot.status())
    elif args.cmd == "sensors":
        print_json(robot.sensors())
    elif args.cmd == "motors":
        print_json(robot.motors_state())
    elif args.cmd == "move":
        print_json(robot.move_motor(args.motor, args.ticks, args.power))
    elif args.cmd == "drive":
        print_json(robot.drive_straight(args.ticks, args.power, args.timeout_ms))
    elif args.cmd == "hold":
        print_json(robot.hold_motor(args.motor, True, args.target, args.power, args.deadband))
    elif args.cmd == "release":
        print_json(robot.hold_motor(args.motor, False))
    elif args.cmd == "raw":
        print_json(robot.set_motors(args.left, args.right, args.lift, args.head, args.ttl_ms))
    elif args.cmd == "stop":
        print_json(robot.stop_motors())
    elif args.cmd == "leds":
        print_json(robot.set_backpack_leds(args.r, args.g, args.b))
    elif args.cmd == "snapshot":
        out = Path(args.out)
        robot.camera_snapshot(out)
        print_json({"ok": True, "path": str(out), "bytes": out.stat().st_size})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
