#!/usr/bin/env python3
"""Minimal stdio MCP server for controlling Vector through vector-hw-api."""

from __future__ import annotations

import argparse
import base64
import json
import struct
import sys
import threading
import zlib
from pathlib import Path
from typing import Any, Callable

from vector_robot import VectorRobot

# Background task manager for run_python_async
tasks: dict[str, dict[str, Any]] = {}
task_lock = threading.Lock()
task_counter = 0


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


# PNG converter for raw BMP camera frames
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


# Async Script execution
def run_python_async(robot: VectorRobot, script: str) -> dict[str, Any]:
    global task_counter
    with task_lock:
        task_counter += 1
        task_id = f"task_{task_counter}"
        tasks[task_id] = {
            "status": "running",
            "exit_code": None,
            "buffer": [],
            "error": None,
        }

    def runner():
        try:
            robot.run_script(
                script.encode("utf-8"),
                print_func=lambda line: tasks[task_id]["buffer"].append(line)
            )
            tasks[task_id]["status"] = "completed"
            tasks[task_id]["exit_code"] = 0
        except Exception as e:
            tasks[task_id]["status"] = "failed"
            tasks[task_id]["error"] = str(e)
            tasks[task_id]["exit_code"] = 1

    t = threading.Thread(target=runner, daemon=True)
    t.start()

    return {
        "status": "running",
        "task_id": task_id,
        "message": "Script is running asynchronously in the background. Use get_task_status to read output.",
    }


def play_animation_async(robot: VectorRobot, name: str, kind: str = "clip") -> dict[str, Any]:
    global task_counter
    with task_lock:
        task_counter += 1
        task_id = f"task_{task_counter}"
        tasks[task_id] = {
            "status": "running",
            "exit_code": None,
            "buffer": [],
            "error": None,
        }

    def runner():
        try:
            result = robot.animations.play(
                name,
                kind=kind,
                print_func=lambda line: tasks[task_id]["buffer"].append(line),
            )
            tasks[task_id]["buffer"].append(json.dumps({"result": result}, indent=2, sort_keys=True) + "\n")
            tasks[task_id]["status"] = "completed"
            tasks[task_id]["exit_code"] = 0 if result.get("ok") else 1
        except Exception as e:
            tasks[task_id]["status"] = "failed"
            tasks[task_id]["error"] = str(e)
            tasks[task_id]["exit_code"] = 1

    t = threading.Thread(target=runner, daemon=True)
    t.start()

    return {
        "status": "running",
        "task_id": task_id,
        "message": "Animation playback is running asynchronously. Use get_task_status to read output.",
    }


def get_task_status(task_id: str) -> dict[str, Any]:
    if task_id not in tasks:
        return {"status": "unknown", "error": f"Task {task_id} not found."}

    # Fetch and clear buffered log lines for incremental streaming behavior
    log_lines = list(tasks[task_id]["buffer"])
    tasks[task_id]["buffer"] = tasks[task_id]["buffer"][len(log_lines):]
    stdout_stderr = "".join(log_lines)

    return {
        "task_id": task_id,
        "status": tasks[task_id]["status"],
        "exit_code": tasks[task_id]["exit_code"],
        "stdout_stderr": stdout_stderr,
        "error": tasks[task_id]["error"],
    }


CONFIG_FILE = Path(__file__).parent / "mcp_config.json"


def load_saved_host() -> str | None:
    if CONFIG_FILE.exists():
        try:
            with open(CONFIG_FILE, "r") as f:
                data = json.load(f)
                return data.get("host")
        except Exception:
            pass
    return None


def save_host(host: str):
    try:
        with open(CONFIG_FILE, "w") as f:
            json.dump({"host": host}, f, indent=4)
    except Exception:
        pass


def list_animations_handler(robot: VectorRobot, args: dict[str, Any]) -> dict[str, Any]:
    data = robot.animations.list()
    needle = str(args.get("filter", "")).strip().lower()
    limit = max(1, min(int(args.get("limit", 40)), 200))

    def keep(item: dict[str, Any]) -> bool:
        return (
            not needle
            or needle in str(item.get("name", "")).lower()
            or needle in str(item.get("path", "")).lower()
            or any(needle in str(clip).lower() for clip in item.get("clips", []))
        )

    return {
        "root": data["root"],
        "clips": [item for item in data["clips"] if keep(item)][:limit],
        "groups": [item for item in data["groups"] if keep(item)][:limit],
        "total_clips": len(data["clips"]),
        "total_groups": len(data["groups"]),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.89")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    saved_host = load_saved_host()
    target_host = saved_host or args.host
    robot = VectorRobot(target_host, args.port)

    tools = [
        tool_schema(
            "set_robot_ip",
            "Change the robot's target IP address dynamically and save it persistently so all future sessions use it automatically.",
            {
                "ip": {"type": "string", "description": "The new target IPv4 address of the robot on the local network (e.g. '192.168.1.93')."}
            },
            ["ip"],
        ),
        tool_schema(
            "get_state",
            "SDK-aligned alias for VectorRobot.get_state(). Read one unified snapshot of robot status, sensors, and motor telemetry. Use this before planning movements. "
            "Returned schema contains keys:\n"
            "- 'sensors': {'accel': {'x', 'y', 'z'} (g-forces), 'gyro': {'x', 'y', 'z'} (angular velocities in deg/s), 'imu_temp': float, 'proximity': {'range_mm', 'status'}, 'touch': [level_0, level_1], 'cliff': [c0, c1, c2, c3], 'buttons': {'power': bool, 'power_hold_ms': int}}\n"
            "- 'motors': array of 4 objects with 'id', 'name', 'position', 'delta', 'moving'\n"
            "- 'buttons': {'power': bool, 'power_hold_ms': int}\n"
            "NOTE: There is NO absolute orientation/yaw sensor on this robot. To turn by a precise angle (e.g. 90 deg), you MUST write a Python script and run it via 'run_python_async' that integrates 'gyro.z' (Z-axis velocity) over time, subtracting the zero-rate bias offset (bias_z = 0.32715845) stored in '/data/gyro_calibration.json'. Formula: angle += (gyro_z - bias_z) * dt.",
            {},
        ),
        tool_schema(
            "drive_distance",
            "SDK-aligned wrapper for VectorRobot.drive_distance(). Drive straight by physical forward-positive encoder ticks, wait until stopped, and return final position/error feedback. Positive ticks move forward; negative ticks reverse.",
            {
                "ticks": {"type": "integer", "description": "Signed relative encoder ticks to drive. Start small, e.g., 100 to 500 ticks."},
                "power": {"type": "number", "minimum": 0.05, "maximum": 1.0, "default": 0.35, "description": "Track power cap. Keep it low (e.g. 0.25 to 0.45) indoors."},
                "timeout": {"type": "number", "minimum": 1.0, "maximum": 60.0, "default": 15.0, "description": "Seconds to wait before aborting movement loop."},
                "tolerance": {"type": "integer", "minimum": 0, "maximum": 200, "default": 25, "description": "Allowed track position tick error tolerance."},
            },
            ["ticks"],
        ),
        tool_schema(
            "drive_raw",
            "SDK-aligned wrapper for VectorRobot.drive_raw(). Send raw left/right track power for a bounded duration, refresh the hardware TTL, then stop motors.",
            {
                "left": {"type": "number", "minimum": -1.0, "maximum": 1.0, "description": "Left track power. Positive moves forward."},
                "right": {"type": "number", "minimum": -1.0, "maximum": 1.0, "description": "Right track power. Positive moves forward."},
                "duration": {"type": "number", "minimum": 0.05, "maximum": 5.0, "description": "Duration in seconds to power the tracks."},
            },
            ["left", "right", "duration"],
        ),
        tool_schema(
            "move_joint",
            "SDK-aligned wrapper for VectorRobot.move_joint(). Move lift or head by relative encoder ticks, wait until stable, and return target vs actual positioning errors.",
            {
                "joint": {"type": "string", "enum": ["lift", "head"], "description": "Joint actuator to move."},
                "ticks": {"type": "integer", "description": "Signed relative encoder ticks. Lift positive moves up; head positive tilts down (direction is calibration-dependent; start small +/-50)."},
                "power": {"type": "number", "minimum": 0.01, "maximum": 1.0, "default": 0.4, "description": "Joint motor power cap."},
                "timeout": {"type": "number", "minimum": 1.0, "maximum": 30.0, "default": 12.0, "description": "Seconds to wait for joint to settle."},
                "tolerance": {"type": "integer", "minimum": 0, "maximum": 100, "default": 15, "description": "Allowed settling tolerance in ticks."},
            },
            ["joint", "ticks"],
        ),
        tool_schema(
            "stop_motors",
            "Instantly abort all active movements, holds, or raw track power commands and stop all motors.",
            {},
        ),
        tool_schema(
            "back_button_pressed",
            "Compatibility wrapper for VectorRobot.back_button_pressed(). Returns whether the physical rear power button is currently pressed.",
            {},
        ),
        tool_schema(
            "power_button_pressed",
            "SDK-aligned wrapper for VectorRobot.power_button_pressed(). Returns whether the physical rear power button is currently pressed.",
            {},
        ),
        tool_schema(
            "set_backpack_leds",
            "Set the backpack LEDs to one solid RGB color (each value 0-255).",
            {
                "r": {"type": "integer", "minimum": 0, "maximum": 255},
                "g": {"type": "integer", "minimum": 0, "maximum": 255},
                "b": {"type": "integer", "minimum": 0, "maximum": 255},
            },
            ["r", "g", "b"],
        ),
        tool_schema(
            "set_backpack_led",
            "Set one backpack LED by index/name while leaving the other LEDs unchanged.",
            {
                "led": {"type": "integer", "minimum": 0, "maximum": 3, "description": "Backpack LED index: 0=back, 1=middle, 2=front, 3=status."},
                "r": {"type": "integer", "minimum": 0, "maximum": 255},
                "g": {"type": "integer", "minimum": 0, "maximum": 255},
                "b": {"type": "integer", "minimum": 0, "maximum": 255},
            },
            ["led", "r", "g", "b"],
        ),
        tool_schema(
            "camera_snapshot",
            "Capture a camera snapshot and return it directly to the model as standard image/png content, plus text metadata about the original captured BMP image.",
            {},
        ),
        tool_schema(
            "list_animations",
            "SDK-aligned wrapper for robot.animations.list(). List DDL vector-animations-build clips and groups available from VECTOR_ANIMATIONS_ROOT on this computer.",
            {
                "filter": {"type": "string", "description": "Optional case-insensitive substring filter applied to clip/group name and path."},
                "limit": {"type": "integer", "minimum": 1, "maximum": 200, "default": 40, "description": "Maximum clips and maximum groups to return."},
            },
        ),
        tool_schema(
            "play_animation_async",
            "SDK-aligned wrapper for robot.animations.play(). Play a DDL animation clip or group asynchronously through /v1/apps/run-script. Use get_task_status to stream output and completion.",
            {
                "name": {"type": "string", "description": "Clip name such as anim_avs_back2listen_03 or group name such as ag_vc_laser_lookdown."},
                "kind": {"type": "string", "enum": ["clip", "group"], "default": "clip", "description": "Whether name refers to a concrete clip or an animation group."},
            },
            ["name"],
        ),
        tool_schema(
            "stop_animation",
            "Stop active animation effects by stopping motors and audio. Display/LED state is left as-is for inspection.",
            {},
        ),
        tool_schema(
            "run_python_async",
            "Upload and run a complete Python script on the robot asynchronously in the background. The robot injects a local vector_robot module, so scripts can use `from vector_robot import VectorRobot`. Immediately returns task_id; use get_task_status to read output.",
            {
                "script": {
                    "type": "string",
                    "description": (
                        "Complete Python script to execute. Must import VectorRobot, wrap movement in try/finally, and call stop_motors() in cleanup. "
                        "NOTE: There is no absolute orientation sensor on this robot. To perform a precision turn, load the gyroscope calibration bias from '/data/gyro_calibration.json' (default bias_z = 0.32715845), "
                        "read raw Z angular velocity from `robot.sensors()['gyro']['z']`, subtract bias_z, and integrate over time: `angle += (gyro_z - bias_z) * dt`. "
                        "Always use time.sleep(0.01) in the loop and read state dynamically."
                    )
                },
            },
            ["script"],
        ),
        tool_schema(
            "get_task_status",
            "Poll the execution status, exit code, errors, and newly buffered stdout/stderr logs of a running background Python script task.",
            {
                "task_id": {"type": "string", "description": "The unique task_id returned by run_python_async."},
            },
            ["task_id"],
        ),
    ]

    def set_robot_ip_handler(a: dict[str, Any]) -> dict[str, Any]:
        ip = str(a["ip"]).strip()
        robot.host = ip
        save_host(ip)
        return {
            "ok": True,
            "message": f"Robot target IP changed to {ip} and saved persistently.",
            "current_host": robot.host,
        }

    handlers: dict[str, Callable[[dict[str, Any]], Any]] = {
        "set_robot_ip": set_robot_ip_handler,
        "get_state": lambda _a: robot.get_state(),
        "drive_distance": lambda a: robot.drive_distance(int(a["ticks"]), float(a.get("power", 0.35)), float(a.get("timeout", 15.0)), int(a.get("tolerance", 25))),
        "drive_raw": lambda a: robot.drive_raw(float(a["left"]), float(a["right"]), float(a["duration"])),
        "move_joint": lambda a: robot.move_joint(str(a["joint"]), int(a["ticks"]), float(a.get("power", 0.4)), float(a.get("timeout", 12.0)), int(a.get("tolerance", 15))),
        "stop_motors": lambda _a: robot.stop_motors(),
        "back_button_pressed": lambda _a: {"pressed": robot.back_button_pressed()},
        "power_button_pressed": lambda _a: {"pressed": robot.power_button_pressed(), "hold_ms": robot.power_button_hold_ms()},
        "set_backpack_leds": lambda a: robot.set_backpack_leds(int(a["r"]), int(a["g"]), int(a["b"])),
        "set_backpack_led": lambda a: robot.set_backpack_led(int(a["led"]), int(a["r"]), int(a["g"]), int(a["b"])),
        "list_animations": lambda a: list_animations_handler(robot, a),
        "play_animation_async": lambda a: play_animation_async(robot, str(a["name"]), str(a.get("kind", "clip"))),
        "stop_animation": lambda _a: robot.animations.stop(),
        "run_python_async": lambda a: run_python_async(robot, str(a["script"])),
        "get_task_status": lambda a: get_task_status(str(a["task_id"])),
        # Backwards-compatible aliases accepted by older docs/prompts. They are
        # intentionally omitted from tools/list so new clients see SDK names.
        "get_robot_state": lambda _a: robot.get_state(),
        "status": lambda _a: robot.status(),
        "sensors": lambda _a: robot.sensors(),
        "motors_state": lambda _a: robot.motors_state(),
        "drive_for": lambda a: robot.drive_raw(float(a["left"]), float(a["right"]), float(a["duration"])),
        "move_lift": lambda a: robot.move_joint("lift", int(a["ticks"]), float(a.get("power", 0.4)), float(a.get("timeout", 12.0)), int(a.get("tolerance", 15))),
        "move_head": lambda a: robot.move_joint("head", int(a["ticks"]), float(a.get("power", 0.4)), float(a.get("timeout", 12.0)), int(a.get("tolerance", 15))),
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
                    "serverInfo": {"name": "wireos-vector", "version": "0.2.2"},
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
