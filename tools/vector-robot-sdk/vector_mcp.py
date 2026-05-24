#!/usr/bin/env python3
"""Minimal stdio MCP server for controlling Vector through vector-hw-api.

This intentionally avoids external Python packages. It implements the JSON-RPC
methods LLM clients need for tool discovery and tool calls:
`initialize`, `tools/list`, and `tools/call`.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any, Callable

from vector_robot import VectorRobot


def tool_schema(name: str, description: str, properties: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    return {
        "name": name,
        "description": description,
        "inputSchema": {
            "type": "object",
            "properties": properties,
            "required": required or [],
        },
    }


def content(value: Any) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": json.dumps(value, indent=2, sort_keys=True)}]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.89")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    robot = VectorRobot(args.host, args.port)

    tools = [
        tool_schema("status", "Read robot status, including API, camera, audio, battery and IMU fields.", {}),
        tool_schema("sensors", "Read raw body sensors: encoders, cliff, touch, proximity and battery.", {}),
        tool_schema("motors_state", "Read current motor encoder state.", {}),
        tool_schema("move_motor", "Move one motor by relative encoder ticks.", {
            "motor": {"type": "integer", "minimum": 0, "maximum": 3},
            "ticks": {"type": "integer"},
            "power": {"type": "number", "minimum": 0.01, "maximum": 1.0, "default": 0.5},
        }, ["motor", "ticks"]),
        tool_schema("hold_motor", "Enable closed-loop encoder hold for one motor.", {
            "motor": {"type": "integer", "minimum": 0, "maximum": 3},
            "target": {"type": "integer", "description": "Optional encoder target. Current position is used when omitted."},
            "power": {"type": "number", "minimum": 0.2, "maximum": 1.0, "default": 0.7},
            "deadband": {"type": "integer", "minimum": 1, "maximum": 100, "default": 6},
        }, ["motor"]),
        tool_schema("release_motor", "Disable closed-loop hold for one motor.", {
            "motor": {"type": "integer", "minimum": 0, "maximum": 3},
        }, ["motor"]),
        tool_schema("stop_motors", "Immediately cancel movement/hold and stop all motors.", {}),
        tool_schema("set_backpack_leds", "Set backpack LEDs to one RGB color.", {
            "r": {"type": "integer", "minimum": 0, "maximum": 255},
            "g": {"type": "integer", "minimum": 0, "maximum": 255},
            "b": {"type": "integer", "minimum": 0, "maximum": 255},
        }, ["r", "g", "b"]),
        tool_schema("camera_snapshot", "Capture a camera snapshot and return byte count. Use the HTTP API directly to fetch image bytes.", {}),
    ]

    handlers: dict[str, Callable[[dict[str, Any]], Any]] = {
        "status": lambda _a: robot.status(),
        "sensors": lambda _a: robot.sensors(),
        "motors_state": lambda _a: robot.motors_state(),
        "move_motor": lambda a: robot.move_motor(int(a["motor"]), int(a["ticks"]), float(a.get("power", 0.5))),
        "hold_motor": lambda a: robot.hold_motor(int(a["motor"]), True, a.get("target"), float(a.get("power", 0.7)), int(a.get("deadband", 6))),
        "release_motor": lambda a: robot.hold_motor(int(a["motor"]), False),
        "stop_motors": lambda _a: robot.stop_motors(),
        "set_backpack_leds": lambda a: robot.set_backpack_leds(int(a["r"]), int(a["g"]), int(a["b"])),
        "camera_snapshot": lambda _a: {"ok": True, "bytes": len(robot.camera_snapshot())},
    }

    for line in sys.stdin:
        try:
            req = json.loads(line)
            method = req.get("method")
            if method == "initialize":
                result = {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "wireos-vector", "version": "0.1.0"},
                }
            elif method == "tools/list":
                result = {"tools": tools}
            elif method == "tools/call":
                params = req.get("params", {})
                name = params.get("name")
                arguments = params.get("arguments") or {}
                if name not in handlers:
                    raise ValueError(f"unknown tool: {name}")
                result = content(handlers[name](arguments))
            else:
                result = {}
            print(json.dumps({"jsonrpc": "2.0", "id": req.get("id"), "result": result}), flush=True)
        except Exception as exc:
            print(json.dumps({
                "jsonrpc": "2.0",
                "id": req.get("id") if "req" in locals() else None,
                "error": {"code": -32000, "message": str(exc)},
            }), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

