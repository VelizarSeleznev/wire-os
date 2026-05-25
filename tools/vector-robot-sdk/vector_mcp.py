#!/usr/bin/env python3
"""Minimal stdio MCP server for controlling Vector through vector-hw-api.

This intentionally avoids external Python packages. It implements the JSON-RPC
methods LLM clients need for tool discovery and tool calls:
`initialize`, `tools/list`, and `tools/call`.
"""

from __future__ import annotations

import argparse
import base64
import json
import struct
import sys
import zlib
from typing import Any, Callable

from vector_robot import VectorRobot


MOTOR_MAP = {
    0: "left_track",
    1: "right_track",
    2: "lift",
    3: "head",
}

ROBOT_HELP = {
    "robot": "Anki Vector running WireOS vector-hw-api over LAN.",
    "host_default": "192.168.1.89",
    "api_port_default": 8080,
    "motor_map": MOTOR_MAP,
    "safe_usage": [
        "Call motors_state before movement if the current position matters.",
        "Use drive_straight for forward/reverse driving instead of issuing separate left/right track commands.",
        "Use move_lift for the lift and move_head for the head instead of guessing raw motor indexes.",
        "Use move_motor only when you explicitly need a raw motor id.",
        "Use small tick values first, then inspect motors_state.",
        "Call stop_motors immediately if motion looks wrong or the user asks to stop.",
    ],
    "movement_notes": {
        "move_motor": "Relative encoder move for motor ids 0..3.",
        "drive_straight": "Synchronized straight track movement. Positive ticks drive forward; negative ticks reverse.",
        "move_lift": "Alias for move_motor motor=2.",
        "move_head": "Alias for move_motor motor=3.",
        "ticks": "Signed relative encoder ticks. Positive/negative direction depends on current robot calibration; use small values first.",
        "power": "0.01..1.0 motor power cap. Prefer 0.2..0.5 for exploratory movements.",
    },
}


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


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def bmp_to_png(image: bytes) -> bytes | None:
    if not image.startswith(b"BM") or len(image) < 54:
        return None
    pixel_offset = struct.unpack_from("<I", image, 10)[0]
    dib_size = struct.unpack_from("<I", image, 14)[0]
    if dib_size < 40:
        return None
    width = struct.unpack_from("<i", image, 18)[0]
    height_signed = struct.unpack_from("<i", image, 22)[0]
    planes = struct.unpack_from("<H", image, 26)[0]
    bpp = struct.unpack_from("<H", image, 28)[0]
    compression = struct.unpack_from("<I", image, 30)[0]
    if planes != 1 or compression != 0 or width <= 0 or height_signed == 0 or bpp not in (24, 32):
        return None

    height = abs(height_signed)
    top_down = height_signed < 0
    src_channels = bpp // 8
    row_stride = ((width * src_channels + 3) // 4) * 4
    if pixel_offset + row_stride * height > len(image):
        return None

    rows = []
    y_range = range(height) if top_down else range(height - 1, -1, -1)
    for y in y_range:
        src = pixel_offset + y * row_stride
        row = bytearray()
        for x in range(width):
            b = image[src + x * src_channels + 0]
            g = image[src + x * src_channels + 1]
            r = image[src + x * src_channels + 2]
            row.extend((r, g, b))
        rows.append(b"\x00" + bytes(row))

    raw = b"".join(rows)
    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(raw, 6))
    png += _png_chunk(b"IEND", b"")
    return bytes(png)


def camera_snapshot_content(robot: VectorRobot) -> dict[str, Any]:
    image = robot.camera_snapshot()
    original_mime_type = "image/bmp" if image.startswith(b"BM") else "application/octet-stream"
    output = bmp_to_png(image) if image.startswith(b"BM") else None
    if output is not None:
        image_content = output
        mime_type = "image/png"
    else:
        image_content = image
        mime_type = original_mime_type
    return {
        "content": [
            {
                "type": "text",
                "text": json.dumps(
                    {
                        "ok": True,
                        "mime_type": mime_type,
                        "bytes": len(image_content),
                        "original_mime_type": original_mime_type,
                        "original_bytes": len(image),
                        "note": "The next content item is the camera image captured from the robot.",
                    },
                    indent=2,
                    sort_keys=True,
                ),
            },
            {
                "type": "image",
                "data": base64.b64encode(image_content).decode("ascii"),
                "mimeType": mime_type,
            },
        ]
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.89")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    robot = VectorRobot(args.host, args.port)

    tools = [
        tool_schema(
            "robot_help",
            "Read this first. Returns the robot command guide, motor map, and safety notes. Motor map: 0=left_track, 1=right_track, 2=lift, 3=head.",
            {},
        ),
        tool_schema(
            "status",
            "Read overall robot status from vector-hw-api: API version, spine connection, display, camera, audio, battery, motors, cliff, proximity, touch, and IMU. Use this to verify the robot is reachable before commanding movement.",
            {},
        ),
        tool_schema(
            "sensors",
            "Read raw body sensors: motor encoders, cliff sensors, touch sensors, proximity/TOF, and battery. Treat proximity/IMU as diagnostic until calibrated.",
            {},
        ),
        tool_schema(
            "motors_state",
            "Read current motor encoder state and names. Motor ids are fixed: 0=left_track, 1=right_track, 2=lift, 3=head. Use this before and after movement commands.",
            {},
        ),
        tool_schema("move_motor", "Move one raw motor by relative encoder ticks. Motor ids: 0=left_track, 1=right_track, 2=lift, 3=head. Prefer move_lift or move_head for those named mechanisms. Use small tick values first and then call motors_state.", {
            "motor": {"type": "integer", "enum": [0, 1, 2, 3], "description": "0=left_track, 1=right_track, 2=lift, 3=head"},
            "ticks": {"type": "integer", "description": "Signed relative encoder ticks. Direction is calibration-dependent; start small, e.g. +/-50 to +/-200 for lift/head checks."},
            "power": {"type": "number", "minimum": 0.01, "maximum": 1.0, "default": 0.5, "description": "Motor power cap. Prefer 0.2..0.5 for exploratory movement."},
        }, ["motor", "ticks"]),
        tool_schema("drive_straight", "Drive both tracks together using one synchronized robot-side encoder command. Positive ticks drive forward; negative ticks reverse. Prefer this over separate left/right move_motor calls for straight movement.", {
            "ticks": {"type": "integer", "description": "Signed straight-drive encoder ticks. Start small, e.g. +/-100 to +/-300, then inspect motors_state and camera_snapshot."},
            "power": {"type": "number", "minimum": 0.01, "maximum": 1.0, "default": 0.35, "description": "Track power cap. Prefer 0.2..0.4 indoors."},
            "timeout_ms": {"type": "integer", "minimum": 250, "maximum": 10000, "default": 10000, "description": "Safety timeout for the background drive command."},
        }, ["ticks"]),
        tool_schema("move_lift", "Move the lift only. This is an alias for move_motor with motor=2. Use this instead of guessing the lift motor id.", {
            "ticks": {"type": "integer", "description": "Signed relative lift encoder ticks. Start small, e.g. +/-50 to +/-200, then inspect motors_state."},
            "power": {"type": "number", "minimum": 0.01, "maximum": 1.0, "default": 0.5, "description": "Lift motor power cap. Prefer 0.2..0.5 for exploratory movement."},
        }, ["ticks"]),
        tool_schema("move_head", "Move the head only. This is an alias for move_motor with motor=3. Use this instead of guessing the head motor id.", {
            "ticks": {"type": "integer", "description": "Signed relative head encoder ticks. Start small, e.g. +/-20 to +/-100, then inspect motors_state."},
            "power": {"type": "number", "minimum": 0.01, "maximum": 1.0, "default": 0.4, "description": "Head motor power cap. Prefer 0.2..0.4 for exploratory movement."},
        }, ["ticks"]),
        tool_schema("hold_motor", "Enable closed-loop encoder hold for one motor. Motor ids: 0=left_track, 1=right_track, 2=lift, 3=head. Holding lift/head can resist gravity after positioning; call release_motor to disable.", {
            "motor": {"type": "integer", "enum": [0, 1, 2, 3], "description": "0=left_track, 1=right_track, 2=lift, 3=head"},
            "target": {"type": "integer", "description": "Optional encoder target. Current position is used when omitted."},
            "power": {"type": "number", "minimum": 0.2, "maximum": 1.0, "default": 0.7, "description": "Hold power cap."},
            "deadband": {"type": "integer", "minimum": 1, "maximum": 100, "default": 6, "description": "Encoder tick deadband around target."},
        }, ["motor"]),
        tool_schema("release_motor", "Disable closed-loop hold for one motor. Motor ids: 0=left_track, 1=right_track, 2=lift, 3=head.", {
            "motor": {"type": "integer", "enum": [0, 1, 2, 3], "description": "0=left_track, 1=right_track, 2=lift, 3=head"},
        }, ["motor"]),
        tool_schema("stop_motors", "Immediately cancel movement/hold and stop all motors. Use this if motion is wrong, unexpected, or the user says stop.", {}),
        tool_schema("set_backpack_leds", "Set backpack LEDs to one RGB color.", {
            "r": {"type": "integer", "minimum": 0, "maximum": 255},
            "g": {"type": "integer", "minimum": 0, "maximum": 255},
            "b": {"type": "integer", "minimum": 0, "maximum": 255},
        }, ["r", "g", "b"]),
        tool_schema(
            "camera_snapshot",
            "Capture a camera snapshot and return it directly to the model as image/png content, plus text metadata about the original robot image.",
            {},
        ),
    ]

    handlers: dict[str, Callable[[dict[str, Any]], Any]] = {
        "robot_help": lambda _a: ROBOT_HELP,
        "status": lambda _a: robot.status(),
        "sensors": lambda _a: robot.sensors(),
        "motors_state": lambda _a: robot.motors_state(),
        "move_motor": lambda a: robot.move_motor(int(a["motor"]), int(a["ticks"]), float(a.get("power", 0.5))),
        "drive_straight": lambda a: robot.drive_straight(int(a["ticks"]), float(a.get("power", 0.35)), int(a.get("timeout_ms", 10000))),
        "move_lift": lambda a: robot.move_motor(2, int(a["ticks"]), float(a.get("power", 0.5))),
        "move_head": lambda a: robot.move_motor(3, int(a["ticks"]), float(a.get("power", 0.4))),
        "hold_motor": lambda a: robot.hold_motor(int(a["motor"]), True, a.get("target"), float(a.get("power", 0.7)), int(a.get("deadband", 6))),
        "release_motor": lambda a: robot.hold_motor(int(a["motor"]), False),
        "stop_motors": lambda _a: robot.stop_motors(),
        "set_backpack_leds": lambda a: robot.set_backpack_leds(int(a["r"]), int(a["g"]), int(a["b"])),
    }

    for line in sys.stdin:
        try:
            req = json.loads(line)
            method = req.get("method")
            if "id" not in req:
                continue
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
                if name == "camera_snapshot":
                    result = camera_snapshot_content(robot)
                elif name in handlers:
                    result = content(handlers[name](arguments))
                else:
                    raise ValueError(f"unknown tool: {name}")
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
