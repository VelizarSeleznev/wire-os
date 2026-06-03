#!/usr/bin/env python3
"""Small HTTP client for the minimal WireOS Vector hardware API."""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


class VectorRobotError(RuntimeError):
    pass


class VectorRobot:
    _RAW_TICK_POWER_SIGN = {
        0: 1.0,
        1: -1.0,
        2: 1.0,
        3: 1.0,
    }

    _MOTOR_NAME_TO_ID = {
        "left": 0,
        "left_track": 0,
        "right": 1,
        "right_track": 1,
        "lift": 2,
        "head": 3,
    }

    _LED_NAME_TO_ID = {
        "back": 0,
        "middle": 1,
        "front": 2,
        "button": 3,
        "status": 3,
    }

    def _resolve_motor(self, motor: int | str) -> int:
        if isinstance(motor, int):
            if motor not in (0, 1, 2, 3):
                raise ValueError(f"Invalid motor ID {motor}; must be 0..3")
            return motor
        key = str(motor).lower().strip()
        if key not in self._MOTOR_NAME_TO_ID:
            raise ValueError(f"Unknown motor name '{motor}'; use left/right/lift/head or 0..3")
        return self._MOTOR_NAME_TO_ID[key]

    def _resolve_led(self, led: int | str) -> int:
        if isinstance(led, int):
            if led not in (0, 1, 2, 3):
                raise ValueError(f"Invalid LED ID {led}; must be 0..3")
            return led
        key = str(led).lower().strip()
        if key not in self._LED_NAME_TO_ID:
            raise ValueError(f"Unknown LED name '{led}'; use front/middle/back/button or 0..3")
        return self._LED_NAME_TO_ID[key]

    def __init__(self, host: str | None = None, port: int = 8080, timeout: float = 5.0):
        if host is None:
            import os
            env_api = os.environ.get("VECTOR_HW_API")
            if env_api:
                from urllib.parse import urlparse
                try:
                    parsed = urlparse(env_api)
                    host = parsed.hostname or "127.0.0.1"
                    if parsed.port:
                        port = parsed.port
                except Exception:
                    host = "127.0.0.1"
            else:
                if os.path.exists("/usr/bin/vector-hw-api"):
                    host = "127.0.0.1"
                else:
                    host = "192.168.1.89"
        self.host = host
        self.port = port
        self.timeout = timeout
        self.motors = MotorGroup(self)
        self.tracks = TrackGroup(self)
        self.animations = DDLAnimationManager(self)
        self.lift = self.motors.lift
        self.head = self.motors.head

        import threading
        self._continuous_motors_active = False
        self._continuous_powers = [0.0, 0.0, 0.0, 0.0]
        self._continuous_lock = threading.Lock()
        self._refresher_thread = None

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    def request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | bytes | None = None,
        content_type: str = "application/json",
    ) -> Any:
        url = self.base_url + path
        data = None
        headers: dict[str, str] = {}
        if isinstance(body, dict):
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = content_type
        elif isinstance(body, bytes):
            data = body
            headers["Content-Type"] = content_type

        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                payload = resp.read()
                ctype = resp.headers.get("Content-Type", "")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            raise VectorRobotError(f"{method} {path} failed: HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise VectorRobotError(f"{method} {path} failed: {exc.reason}") from exc

        if "application/json" in ctype:
            return json.loads(payload.decode("utf-8"))
        return payload

    def status(self) -> dict[str, Any]:
        return self.request("GET", "/v1/status")

    def sensors(self) -> dict[str, Any]:
        return self.request("GET", "/v1/sensors")

    def back_button_pressed(self) -> bool:
        """Return True when the robot's rear power button is currently pressed.

        Kept for compatibility with older minimal-firmware docs that called the
        same physical button "back".
        """
        buttons = self.sensors().get("buttons") or {}
        return bool(buttons.get("power", buttons.get("back")))

    def power_button_pressed(self) -> bool:
        """Return True when the robot's rear power button is currently pressed."""
        buttons = self.sensors().get("buttons") or {}
        return bool(buttons.get("power", buttons.get("back")))

    def power_button_hold_ms(self) -> int:
        """Return how long the rear power button has been held, in milliseconds."""
        buttons = self.sensors().get("buttons") or {}
        return int(buttons.get("power_hold_ms", 0) or 0)

    def motors_state(self) -> dict[str, Any]:
        return self.request("GET", "/v1/motors/state")

    def set_motors(self, left: float = 0, right: float = 0, lift: float = 0, head: float = 0, ttl_ms: int = 250) -> dict[str, Any]:
        return self.request("POST", "/v1/motors", {
            "left": left,
            "right": right,
            "lift": lift,
            "head": head,
            "ttl_ms": ttl_ms,
        })

    def _set_one_motor(self, motor: int | str, power: float, ttl_ms: int = 120) -> dict[str, Any]:
        motor = self._resolve_motor(motor)
        powers = [0.0, 0.0, 0.0, 0.0]
        powers[motor] = power
        return self.set_motors(powers[0], powers[1], powers[2], powers[3], ttl_ms)

    @staticmethod
    def _track_deltas(before: list[dict[str, Any]], after: list[dict[str, Any]]) -> dict[str, int]:
        raw_left = int(after[0]["position"]) - int(before[0]["position"])
        raw_right = int(after[1]["position"]) - int(before[1]["position"])
        return {
            "left_raw": raw_left,
            "right_raw": raw_right,
            "left_forward": raw_left,
            "right_forward": -raw_right,
        }

    def set_motors_for(
        self,
        left: float = 0,
        right: float = 0,
        lift: float = 0,
        head: float = 0,
        duration: float = 0.25,
        ttl_ms: int = 180,
        refresh_interval: float = 0.05,
    ) -> dict[str, Any]:
        duration = max(0.0, min(float(duration), 5.0))
        ttl_ms = max(50, min(int(ttl_ms), 500))
        before_state = self.motors_state()
        before = before_state.get("motors") or before_state.get("motor") or []
        deadline = time.monotonic() + duration
        sends = 0
        try:
            while time.monotonic() < deadline:
                self.set_motors(left, right, lift, head, ttl_ms)
                sends += 1
                time.sleep(min(refresh_interval, max(0.0, deadline - time.monotonic())))
        finally:
            self.stop_motors()
        time.sleep(0.2)
        after_state = self.motors_state()
        after = after_state.get("motors") or after_state.get("motor") or []
        track_deltas = self._track_deltas(before, after) if len(before) >= 2 and len(after) >= 2 else {}
        return {
            "ok": True,
            "duration": duration,
            "ttl_ms": ttl_ms,
            "sends": sends,
            "command": {"left": left, "right": right, "lift": lift, "head": head},
            "track_deltas": track_deltas,
            "before": before,
            "after": after,
        }

    def move_motor(
        self,
        motor: int | str,
        ticks: int,
        power: float = 0.5,
        min_power: float | None = None,
        tolerance: int | None = None,
        timeout_ms: int | None = None,
    ) -> dict[str, Any]:
        motor = self._resolve_motor(motor)
        body: dict[str, Any] = {"motor": motor, "ticks": ticks, "power": power}
        if min_power is not None:
            body["min_power"] = min_power
        if tolerance is not None:
            body["tolerance"] = tolerance
        if timeout_ms is not None:
            body["timeout_ms"] = timeout_ms
        return self.request("POST", "/v1/motors/position", body)

    def wait_motor_idle(self, motor: int | str, timeout: float = 12.0, poll_interval: float = 0.1) -> dict[str, Any]:
        motor = self._resolve_motor(motor)
        deadline = time.monotonic() + timeout
        last_state: dict[str, Any] | None = None
        stable_samples = 0
        while time.monotonic() < deadline:
            state = self.motors_state()
            motors = state.get("motors") or state.get("motor") or []
            if len(motors) <= motor:
                raise VectorRobotError(f"motor {motor} missing from /v1/motors/state")
            last_state = motors[motor]
            moving = bool(last_state.get("moving")) or int(last_state.get("delta", 0)) != 0
            stable_samples = 0 if moving else stable_samples + 1
            if stable_samples >= 3:
                return {"ok": True, "timed_out": False, "state": last_state}
            time.sleep(poll_interval)
        return {"ok": False, "timed_out": True, "state": last_state}

    def move_motor_and_wait(
        self,
        motor: int | str,
        ticks: int,
        power: float = 0.5,
        timeout: float = 12.0,
        tolerance: int = 15,
    ) -> dict[str, Any]:
        motor = self._resolve_motor(motor)
        before_state = self.motors_state()
        before_motors = before_state.get("motors") or before_state.get("motor") or []
        if len(before_motors) <= motor:
            raise VectorRobotError(f"motor {motor} missing from /v1/motors/state")
        before = before_motors[motor]
        start_position = int(before.get("position", 0))
        target = start_position + ticks
        command = self.move_motor(motor, ticks, power, tolerance=tolerance, timeout_ms=int(timeout * 1000))
        time.sleep(0.2)
        wait = self.wait_motor_idle(motor, timeout)
        after = wait.get("state") or {}
        after_position = int(after.get("position", start_position))
        error_ticks = after_position - target
        within_tolerance = abs(error_ticks) <= tolerance
        return {
            "ok": bool(command.get("ok")) and not bool(wait.get("timed_out")) and within_tolerance,
            "command": command,
            "motor": motor,
            "ticks": ticks,
            "start_position": start_position,
            "target_position": target,
            "final_position": after_position,
            "moved_ticks": after_position - start_position,
            "error_ticks": error_ticks,
            "tolerance_ticks": tolerance,
            "within_tolerance": within_tolerance,
            "timed_out": bool(wait.get("timed_out")),
            "before": before,
            "after": after,
        }

    def move_motor_precise(
        self,
        motor: int | str,
        ticks: int,
        power: float = 0.25,
        tolerance: int = 5,
        timeout: float = 8.0,
        hold: bool = False,
        min_power: float = 0.12,
    ) -> dict[str, Any]:
        motor = self._resolve_motor(motor)
        if ticks == 0:
            raise VectorRobotError("ticks must be non-zero")
        if hold and motor in (0, 1):
            raise VectorRobotError("hold is only allowed for lift/head until track hold is revalidated")

        power = max(0.01, min(abs(power), 1.0))
        min_power = max(0.01, min(abs(min_power), power))
        tolerance = max(0, int(tolerance))
        deadline = time.monotonic() + max(0.1, timeout)

        before_state = self.motors_state()
        before_motors = before_state.get("motors") or before_state.get("motor") or []
        if len(before_motors) <= motor:
            raise VectorRobotError(f"motor {motor} missing from /v1/motors/state")
        start = int(before_motors[motor]["position"])
        target = start + int(ticks)
        samples = []
        stable = 0
        sends = 0

        try:
            while time.monotonic() < deadline:
                state = self.motors_state()
                motors = state.get("motors") or state.get("motor") or []
                cur = int(motors[motor]["position"])
                error = target - cur
                samples.append({"position": cur, "error": error})

                if abs(error) <= tolerance:
                    self.stop_motors()
                    stable += 1
                    if stable >= 3:
                        break
                    time.sleep(0.08)
                    continue

                stable = 0
                direction = 1.0 if error > 0 else -1.0
                # Scale down near target but keep enough power to overcome static friction.
                magnitude = min(power, max(min_power, abs(error) * 0.004))
                api_power = direction * self._RAW_TICK_POWER_SIGN[motor] * magnitude
                self._set_one_motor(motor, api_power, 180)
                sends += 1
                time.sleep(0.10)
        finally:
            self.stop_motors()

        time.sleep(0.2)
        after_state = self.motors_state()
        after_motors = after_state.get("motors") or after_state.get("motor") or []
        final = int(after_motors[motor]["position"])
        error_ticks = final - target
        within_tolerance = abs(error_ticks) <= tolerance

        hold_result = None
        if hold and within_tolerance:
            hold_result = {
                "ok": False,
                "error": "robot-side hold is disabled in move_motor_precise because the deployed hold controller oscillates under load",
            }

        return {
            "ok": within_tolerance,
            "motor": motor,
            "ticks": ticks,
            "start_position": start,
            "target_position": target,
            "final_position": final,
            "moved_ticks": final - start,
            "error_ticks": error_ticks,
            "tolerance_ticks": tolerance,
            "within_tolerance": within_tolerance,
            "hold_enabled": bool(hold_result),
            "hold": hold_result,
            "sends": sends,
            "samples": samples[-20:],
            "before": before_motors[motor],
            "after": after_motors[motor],
        }

    def drive_straight(
        self,
        ticks: int,
        power: float = 0.45,
        timeout_ms: int = 10000,
        min_power: float | None = None,
        tolerance: int | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {
            "ticks": ticks,
            "power": power,
            "timeout_ms": timeout_ms,
        }
        if min_power is not None:
            body["min_power"] = min_power
        if tolerance is not None:
            body["tolerance"] = tolerance
        return self.request("POST", "/v1/motors/drive", body)

    def hold_motor(self, motor: int | str, enabled: bool = True, target: int | None = None, power: float = 0.7, deadband: int = 6) -> dict[str, Any]:
        motor = self._resolve_motor(motor)
        body: dict[str, Any] = {"motor": motor, "enabled": 1 if enabled else 0}
        if enabled:
            body.update({"power": power, "deadband": deadband})
            if target is not None:
                body["target"] = target
        return self.request("POST", "/v1/motors/hold", body)

    def stop_motors(self) -> dict[str, Any]:
        with self._continuous_lock:
            self._continuous_motors_active = False
            self._continuous_powers = [0.0, 0.0, 0.0, 0.0]
        return self.request("POST", "/v1/motors/stop")

    def _motor_refresher_loop(self):
        while True:
            with self._continuous_lock:
                if not self._continuous_motors_active:
                    break
                left, right, lift, head = self._continuous_powers
            try:
                self.set_motors(left=left, right=right, lift=lift, head=head, ttl_ms=350)
            except Exception:
                pass
            time.sleep(0.15)

    def set_motors_continuous(self, left: float = 0.0, right: float = 0.0, lift: float = 0.0, head: float = 0.0):
        """Set motor powers continuously in a background thread until stopped or set to 0."""
        if left == 0.0 and right == 0.0 and lift == 0.0 and head == 0.0:
            with self._continuous_lock:
                self._continuous_motors_active = False
                self._continuous_powers = [0.0, 0.0, 0.0, 0.0]
            self.stop_motors()
            return

        with self._continuous_lock:
            self._continuous_powers = [left, right, lift, head]
            start_thread = not self._continuous_motors_active
            self._continuous_motors_active = True

        if start_thread:
            import threading
            self._refresher_thread = threading.Thread(target=self._motor_refresher_loop, daemon=True)
            self._refresher_thread.start()

    def stop_motors_continuous(self):
        """Stop the background continuous motor power thread and zero all motors."""
        with self._continuous_lock:
            self._continuous_motors_active = False
            self._continuous_powers = [0.0, 0.0, 0.0, 0.0]
        self.stop_motors()

    def set_backpack_leds(self, r: int, g: int, b: int) -> dict[str, Any]:
        return self.request("POST", "/v1/leds/backpack", {"r": r, "g": g, "b": b})

    def set_backpack_led(self, led: int | str, r: int, g: int, b: int) -> dict[str, Any]:
        led = self._resolve_led(led)
        return self.request("POST", "/v1/leds/backpack", {"led": led, "r": r, "g": g, "b": b})

    def display_init(self) -> dict[str, Any]:
        return self.request("POST", "/v1/display/init")

    def camera_snapshot(self, out: str | Path | None = None) -> bytes:
        image = self.request("GET", "/v1/camera/snapshot")
        if not isinstance(image, bytes):
            raise VectorRobotError("camera snapshot returned JSON instead of image bytes")
        if out is not None:
            Path(out).write_bytes(image)
        return image

    def run_script(self, script_bytes: bytes, print_func=None):
        """Uploads and runs a Python script, streaming its stdout/stderr in real-time."""
        url = self.base_url + "/v1/apps/run-script"
        req = urllib.request.Request(url, data=script_bytes, headers={"Content-Type": "text/plain"}, method="POST")
        output: list[str] = []
        try:
            with urllib.request.urlopen(req, timeout=None) as resp:
                while True:
                    line = resp.readline()
                    if not line:
                        break
                    decoded = line.decode("utf-8", "replace")
                    output.append(decoded)
                    if print_func:
                        print_func(decoded)
                    else:
                        print(decoded, end="", flush=True)
                return "".join(output)
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            raise VectorRobotError(f"run-script failed: HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise VectorRobotError(f"run-script failed: {exc.reason}") from exc

    # ================= Unified AI-Optimized Methods =================

    def get_state(self) -> dict[str, Any]:
        """Unified state snapshot combining status, sensors, and motor state."""
        try:
            status = self.status()
        except Exception:
            status = {}
        try:
            sensors = self.sensors()
        except Exception:
            sensors = {}
        try:
            motors = self.motors_state()
        except Exception:
            motors = {}

        return {
            "status": status,
            "sensors": sensors,
            "motors": motors.get("motors") or motors.get("motor") or [],
            "buttons": sensors.get("buttons") or {},
        }

    def drive_distance(
        self,
        ticks: int,
        power: float = 0.45,
        timeout: float = 15.0,
        tolerance: int = 12,
    ) -> dict[str, Any]:
        """Drive straight for ticks, blocking until stopped, and return feedback."""
        timeout_ms = int(timeout * 1000)

        # Read starting state
        start_state = self.get_state()
        motors = start_state.get("motors") or []
        if len(motors) < 2:
            raise VectorRobotError("Cannot read track encoders from state")

        start_left = int(motors[0].get("position", 0))
        start_right = int(motors[1].get("position", 0))

        # Target calculations: left increases raw ticks, right decreases raw ticks for forward motion.
        target_left = start_left + ticks
        target_right = start_right - ticks

        # Start motion
        response = self.drive_straight(ticks, power, timeout_ms, tolerance=tolerance)

        # Wait loop
        deadline = time.monotonic() + timeout
        stable_count = 0
        final_left = start_left
        final_right = start_right
        timed_out = False

        while time.monotonic() < deadline:
            time.sleep(0.15)
            state = self.get_state()
            motors = state.get("motors") or []
            if len(motors) < 2:
                continue

            final_left = int(motors[0].get("position", 0))
            final_right = int(motors[1].get("position", 0))

            left_moving = bool(motors[0].get("moving")) or int(motors[0].get("delta", 0)) != 0
            right_moving = bool(motors[1].get("moving")) or int(motors[1].get("delta", 0)) != 0

            if not left_moving and not right_moving:
                stable_count += 1
                if stable_count >= 3:
                    break
            else:
                stable_count = 0
        else:
            timed_out = True

        left_delta = final_left - start_left
        right_delta = final_right - start_right

        error_left = final_left - target_left
        error_right = final_right - target_right

        return {
            "ok": not timed_out and abs(error_left) <= tolerance and abs(error_right) <= tolerance,
            "requested_ticks": ticks,
            "left_delta_raw": left_delta,
            "right_delta_raw": right_delta,
            "left_forward": left_delta,
            "right_forward": -right_delta,
            "overshoot_error": {
                "left": error_left,
                "right": error_right,
            },
            "final_positions": {
                "left": final_left,
                "right": final_right,
            },
            "timed_out": timed_out,
            "api_response": response,
        }

    def drive_raw(
        self,
        left: float,
        right: float,
        duration: float,
        ttl_ms: int = 200,
    ) -> dict[str, Any]:
        """Send raw track power for a set duration, then stop, returning deltas."""
        duration = max(0.05, min(float(duration), 5.0))
        ttl_ms = max(50, min(int(ttl_ms), 500))

        start_state = self.get_state()
        motors = start_state.get("motors") or []
        start_left = int(motors[0].get("position", 0)) if len(motors) >= 1 else 0
        start_right = int(motors[1].get("position", 0)) if len(motors) >= 2 else 0

        deadline = time.monotonic() + duration
        sends = 0
        try:
            while time.monotonic() < deadline:
                self.set_motors(left=left, right=right, lift=0.0, head=0.0, ttl_ms=ttl_ms)
                sends += 1
                time.sleep(0.05)
        finally:
            self.stop_motors()

        time.sleep(0.2)
        end_state = self.get_state()
        end_motors = end_state.get("motors") or []
        final_left = int(end_motors[0].get("position", start_left)) if len(end_motors) >= 1 else start_left
        final_right = int(end_motors[1].get("position", start_right)) if len(end_motors) >= 2 else start_right

        left_delta = final_left - start_left
        right_delta = final_right - start_right

        return {
            "ok": True,
            "duration": duration,
            "sends": sends,
            "left_delta_raw": left_delta,
            "right_delta_raw": right_delta,
            "left_forward": left_delta,
            "right_forward": -right_delta,
        }

    def move_joint(
        self,
        joint: str,
        ticks: int,
        power: float = 0.4,
        timeout: float = 12.0,
        tolerance: int = 15,
    ) -> dict[str, Any]:
        """Move a specific joint (lift or head), blocking until stable, returning feedback."""
        joint_name = str(joint).lower().strip()
        if joint_name not in ("lift", "head"):
            raise ValueError("joint must be 'lift' or 'head'")

        motor_id = 2 if joint_name == "lift" else 3

        # Read initial position
        start_state = self.get_state()
        motors = start_state.get("motors") or []
        if len(motors) <= motor_id:
            raise VectorRobotError(f"Motor {motor_id} ({joint_name}) missing from status")

        start_pos = int(motors[motor_id].get("position", 0))
        target_pos = start_pos + ticks

        # Command move
        response = self.move_motor(motor_id, ticks, power)

        # Wait loop
        deadline = time.monotonic() + timeout
        stable_count = 0
        final_pos = start_pos
        timed_out = False

        while time.monotonic() < deadline:
            time.sleep(0.1)
            state = self.get_state()
            motors = state.get("motors") or []
            if len(motors) <= motor_id:
                continue
            final_pos = int(motors[motor_id].get("position", 0))
            moving = bool(motors[motor_id].get("moving")) or int(motors[motor_id].get("delta", 0)) != 0

            if not moving:
                stable_count += 1
                if stable_count >= 3:
                    break
            else:
                stable_count = 0
        else:
            timed_out = True

        error = final_pos - target_pos

        return {
            "ok": not timed_out and abs(error) <= tolerance,
            "joint": joint_name,
            "requested_ticks": ticks,
            "start_position": start_pos,
            "target_position": target_pos,
            "final_position": final_pos,
            "moved_ticks": final_pos - start_pos,
            "error_ticks": error,
            "timed_out": timed_out,
            "api_response": response,
        }


class DDLAnimationManager:
    """Play DDL `vector-animations-build` JSON assets through robot scripts.

    This is a client-side prototype layer. It reads animation JSON from the
    local machine, generates a small Python player, and runs it on the robot via
    `/v1/apps/run-script`.
    """

    def __init__(self, robot: VectorRobot, assets_root: str | Path | None = None):
        self.robot = robot
        if assets_root is None:
            import os
            assets_root = os.environ.get("VECTOR_ANIMATIONS_ROOT", "/tmp/vector-animations-build/assets")
        self.assets_root = Path(assets_root)

    def _walk_json(self, subdir: str) -> list[Path]:
        root = self.assets_root / subdir
        if not root.is_dir():
            return []
        return sorted(root.rglob("*.json"))

    @staticmethod
    def _frame_summary(name: str, rel_path: str, frames: list[dict[str, Any]]) -> dict[str, Any]:
        tracks: dict[str, int] = {}
        duration_ms = 0
        for frame in frames:
            track = str(frame.get("Name") or "Unknown")
            tracks[track] = tracks.get(track, 0) + 1
            duration_ms = max(
                duration_ms,
                int(float(frame.get("triggerTime_ms") or 0)) + int(float(frame.get("durationTime_ms") or 0)),
            )
        parts = rel_path.split("/")
        return {
            "type": "clip",
            "name": name,
            "path": rel_path,
            "group": parts[1] if len(parts) > 2 else "",
            "duration_ms": duration_ms,
            "frame_count": len(frames),
            "tracks": tracks,
        }

    def list_clips(self) -> list[dict[str, Any]]:
        clips: list[dict[str, Any]] = []
        for path in self._walk_json("animations"):
            try:
                data = json.loads(path.read_text())
                name = next(iter(data.keys()))
                frames = data.get(name)
                if not isinstance(frames, list):
                    continue
                item = self._frame_summary(name, str(path.relative_to(self.assets_root)), frames)
                item["_file"] = str(path)
                clips.append(item)
            except Exception:
                continue
        return sorted(clips, key=lambda item: item["name"])

    def list_groups(self) -> list[dict[str, Any]]:
        groups: list[dict[str, Any]] = []
        for path in self._walk_json("animationGroups"):
            try:
                data = json.loads(path.read_text())
                entries = data.get("Animations")
                if not isinstance(entries, list) or not entries:
                    continue
                rel_path = str(path.relative_to(self.assets_root))
                clips = [str(entry.get("Name") or "") for entry in entries if entry.get("Name")]
                parts = rel_path.split("/")
                groups.append({
                    "type": "group",
                    "name": path.stem,
                    "path": rel_path,
                    "group": parts[1] if len(parts) > 2 else "",
                    "clip_count": len(clips),
                    "total_weight": sum(float(entry.get("Weight") or 0) for entry in entries),
                    "clips": clips[:12],
                    "_file": str(path),
                })
            except Exception:
                continue
        return sorted(groups, key=lambda item: item["name"])

    def list(self) -> dict[str, Any]:
        clips = [{k: v for k, v in item.items() if k != "_file"} for item in self.list_clips()]
        groups = [{k: v for k, v in item.items() if k != "_file"} for item in self.list_groups()]
        return {"root": str(self.assets_root), "clips": clips, "groups": groups}

    def _resolve(self, name: str, kind: str = "clip") -> tuple[dict[str, Any], dict[str, Any] | None]:
        clips = self.list_clips()
        clip_by_name = {item["name"]: item for item in clips}
        if kind != "group":
            for item in clips:
                if item["name"] == name or item["path"] == name:
                    return item, None

        for group in self.list_groups():
            if group["name"] != name and group["path"] != name:
                continue
            data = json.loads(Path(group["_file"]).read_text())
            entries = [
                entry for entry in data.get("Animations", [])
                if str(entry.get("Name") or "") in clip_by_name
            ]
            if not entries:
                raise VectorRobotError(f"animation group {name!r} has no available clips")
            entries.sort(key=lambda entry: (-float(entry.get("Weight") or 0), str(entry.get("Name") or "")))
            selected = clip_by_name[str(entries[0]["Name"])]
            return selected, {"group": group, "choice": entries[0]}

        raise VectorRobotError(f"animation {name!r} not found under {self.assets_root}")

    @staticmethod
    def _compact_frame(frame: dict[str, Any]) -> dict[str, Any]:
        name = frame.get("Name")
        base = {
            "n": name,
            "t": int(float(frame.get("triggerTime_ms") or 0)),
            "d": int(float(frame.get("durationTime_ms") or 0)),
        }
        if name == "LiftHeightKeyFrame":
            return {**base, "h": float(frame.get("height_mm") or 0)}
        if name == "HeadAngleKeyFrame":
            return {**base, "a": float(frame.get("angle_deg") or 0)}
        if name == "BodyMotionKeyFrame":
            return {**base, "r": str(frame.get("radius_mm") or "STRAIGHT"), "s": float(frame.get("speed") or 0)}
        if name == "BackpackLightsKeyFrame":
            return {**base, "f": frame.get("Front") or frame.get("Left") or [], "m": frame.get("Middle") or [], "b": frame.get("Back") or frame.get("Right") or []}
        if name == "ProceduralFaceKeyFrame":
            left = frame.get("leftEye")[:4] if isinstance(frame.get("leftEye"), list) else []
            right = frame.get("rightEye")[:4] if isinstance(frame.get("rightEye"), list) else []
            return {
                **base,
                "l": [float(v) for v in left],
                "e": [float(v) for v in right],
                "sx": float(frame.get("faceScaleX") or 1),
                "sy": float(frame.get("faceScaleY") or 1),
                "cx": float(frame.get("faceCenterX") or 0),
                "cy": float(frame.get("faceCenterY") or 0),
            }
        if name == "FaceAnimationKeyFrame":
            return {**base, "anim": str(frame.get("animName") or "")}
        if name == "RobotAudioKeyFrame":
            return {**base, "audio": True}
        if name == "EventKeyFrame":
            return {**base, "event": str(frame.get("event_id") or "")}
        return base

    @staticmethod
    def _build_script(animation_name: str, events: list[dict[str, Any]]) -> str:
        events_json = json.dumps(events)
        name_json = json.dumps(animation_name)
        return f"""
from vector_robot import VectorRobot
import json, time

EVENTS = json.loads({json.dumps(events_json)})
ANIMATION_NAME = {name_json}
FACE_W = 184
FACE_H = 96
LIFT_ZERO_RAW = -4
LIFT_TICKS_PER_MM = 190.0 / 92.0
HEAD_MIN_DEG = -22.0
HEAD_ZERO_RAW = -320
HEAD_TICKS_PER_DEG = 554.0 / 67.0
TRACK_TICKS_PER_MM = 8.0
robot = VectorRobot()

def clamp(v, lo, hi):
    return max(lo, min(hi, v))

def motor_pos(i):
    ms = robot.motors_state().get("motors") or []
    return int(ms[i].get("position", 0)) if len(ms) > i else 0

def post_json(path, body):
    return robot.request("POST", path, body)

def move_abs(motor, target, power, timeout_ms, tolerance=8):
    delta = int(round(target - motor_pos(motor)))
    if abs(delta) <= tolerance:
        return
    post_json("/v1/motors/position", {{"motor": motor, "ticks": delta, "power": power, "min_power": 0.08, "tolerance": tolerance, "timeout_ms": max(350, int(timeout_ms))}})

def lift_target_raw(height_mm):
    return LIFT_ZERO_RAW + clamp(float(height_mm), 0.0, 92.0) * LIFT_TICKS_PER_MM

def head_target_raw(angle_deg):
    angle = clamp(float(angle_deg), HEAD_MIN_DEG, 45.0)
    return HEAD_ZERO_RAW + (angle - HEAD_MIN_DEG) * HEAD_TICKS_PER_DEG

def color3(vals):
    vals = list(vals or [])
    while len(vals) < 3:
        vals.append(0.0)
    if max(vals[:3] or [0]) <= 1.0:
        vals = [v * 255.0 for v in vals]
    return [int(clamp(round(v), 0, 255)) for v in vals[:3]]

def set_backpack(ev):
    front, middle, back = color3(ev.get("f")), color3(ev.get("m")), color3(ev.get("b"))
    for led, color in ((0, back), (1, middle), (2, front)):
        post_json("/v1/leds/backpack", {{"led": led, "r": color[0], "g": color[1], "b": color[2]}})

def rgb565(r, g, b):
    v = ((int(r) >> 3) << 11) | ((int(g) >> 2) << 5) | (int(b) >> 3)
    return v & 255, (v >> 8) & 255

def fill_ellipse(buf, cx, cy, rx, ry, color):
    if rx <= 0 or ry <= 0:
        return
    lo = rgb565(*color)
    for y in range(max(0, int(cy - ry - 1)), min(FACE_H - 1, int(cy + ry + 1)) + 1):
        dy = (y - cy) / ry
        for x in range(max(0, int(cx - rx - 1)), min(FACE_W - 1, int(cx + rx + 1)) + 1):
            dx = (x - cx) / rx
            if dx * dx + dy * dy <= 1.0:
                idx = (y * FACE_W + x) * 2
                buf[idx], buf[idx + 1] = lo[0], lo[1]

def draw_eye(buf, eye, face_cx, face_cy, sx, sy):
    ex = float(eye[0]) if len(eye) > 0 else 0.0
    ey = float(eye[1]) if len(eye) > 1 else 0.0
    ew = float(eye[2]) if len(eye) > 2 else 1.5
    eh = float(eye[3]) if len(eye) > 3 else 1.1
    cx = FACE_W / 2 + face_cx + ex * 4.0 * sx
    cy = FACE_H / 2 + face_cy + ey * 3.0 * sy
    rx = clamp(abs(ew) * 12.0 * sx, 5.0, 34.0)
    ry = clamp(abs(eh) * 12.0 * sy, 4.0, 28.0)
    fill_ellipse(buf, cx, cy, rx + 2, ry + 2, (0, 32, 42))
    fill_ellipse(buf, cx, cy, rx, ry, (0, 238, 255))
    fill_ellipse(buf, cx + rx * 0.25, cy - ry * 0.25, max(2, rx * 0.18), max(2, ry * 0.18), (210, 255, 255))

def render_face(ev):
    buf = bytearray(FACE_W * FACE_H * 2)
    sx = clamp(float(ev.get("sx", 1.0)), 0.4, 1.8)
    sy = clamp(float(ev.get("sy", 1.0)), 0.4, 1.8)
    cx = clamp(float(ev.get("cx", 0.0)), -45.0, 45.0)
    cy = clamp(float(ev.get("cy", 0.0)), -25.0, 25.0)
    draw_eye(buf, ev.get("l") or [], cx, cy, sx, sy)
    draw_eye(buf, ev.get("e") or [], cx, cy, sx, sy)
    robot.request("POST", "/v1/display/frame", bytes(buf), content_type="application/octet-stream")

def body_motion(ev):
    dur = max(40, int(ev.get("d") or 80))
    speed = float(ev.get("s") or 0.0)
    radius = str(ev.get("r") or "STRAIGHT").upper()
    if radius == "STRAIGHT" and abs(speed) >= 1:
        ticks = int(round(speed * (dur / 1000.0) * TRACK_TICKS_PER_MM))
        if abs(ticks) >= 8:
            post_json("/v1/motors/drive", {{"ticks": ticks, "power": min(0.45, max(0.25, abs(speed) / 350.0)), "min_power": 0.25, "tolerance": 12, "timeout_ms": dur + 650}})
            return
    p = clamp(speed / 350.0, -0.55, 0.55)
    if radius == "TURN_IN_PLACE":
        robot.set_motors(left=-p, right=p, ttl_ms=dur + 120)
    else:
        robot.set_motors(left=p, right=p, ttl_ms=dur + 120)

def handle(ev):
    n = ev.get("n")
    d = max(250, int(ev.get("d") or 250) + 500)
    if n == "LiftHeightKeyFrame":
        move_abs(2, lift_target_raw(ev.get("h", 0)), 0.35, d, 10)
    elif n == "HeadAngleKeyFrame":
        move_abs(3, head_target_raw(ev.get("a", 0)), 0.30, d, 10)
    elif n == "BackpackLightsKeyFrame":
        set_backpack(ev)
    elif n == "ProceduralFaceKeyFrame":
        render_face(ev)
    elif n == "BodyMotionKeyFrame":
        body_motion(ev)
    elif n in ("FaceAnimationKeyFrame", "RobotAudioKeyFrame", "EventKeyFrame", "RecordHeadingKeyFrame", "TurnToRecordedHeadingKeyFrame"):
        print("unsupported_track", n, ev.get("anim") or ev.get("event") or "")

def main():
    print("animation_start", ANIMATION_NAME, "events", len(EVENTS))
    robot.request("POST", "/v1/display/init", {{}})
    start = time.monotonic()
    for ev in sorted(EVENTS, key=lambda x: int(x.get("t") or 0)):
        delay = start + int(ev.get("t") or 0) / 1000.0 - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        handle(ev)
    end_ms = max([int(e.get("t") or 0) + int(e.get("d") or 0) for e in EVENTS] or [0])
    remain = start + end_ms / 1000.0 - time.monotonic()
    if remain > 0:
        time.sleep(min(remain, 2.0))
    robot.stop_motors()
    print("animation_done", ANIMATION_NAME)

try:
    main()
finally:
    try:
        robot.stop_motors()
    except Exception:
        pass
"""

    def play(self, name: str, kind: str = "clip", print_func=None) -> dict[str, Any]:
        clip, group_info = self._resolve(name, kind)
        data = json.loads(Path(clip["_file"]).read_text())
        animation_name = next(iter(data.keys()))
        frames = data.get(animation_name)
        if not isinstance(frames, list):
            raise VectorRobotError(f"animation {animation_name!r} has no frame list")
        events = [self._compact_frame(frame) for frame in frames if frame.get("Name")]
        script = self._build_script(animation_name, events).encode("utf-8")
        output_parts: list[str] = []

        def collect(chunk: str):
            output_parts.append(chunk)
            if print_func:
                print_func(chunk)

        if group_info:
            line = f"animation_group {group_info['group']['name']} selected {clip['name']} weight {float(group_info['choice'].get('Weight') or 0):g}\n"
            collect(line)
        output = self.robot.run_script(script, print_func=collect)
        public_group = None
        if group_info:
            public_group = {
                "group": {k: v for k, v in group_info["group"].items() if k != "_file"},
                "choice": group_info["choice"],
            }
        return {
            "ok": "animation_done" in (output or "".join(output_parts)),
            "name": animation_name,
            "requested": name,
            "kind": kind,
            "clip": {k: v for k, v in clip.items() if k != "_file"},
            "group": public_group,
            "output": "".join(output_parts) if output_parts else output,
        }

    def stop(self) -> dict[str, Any]:
        motors = self.robot.stop_motors()
        try:
            audio = self.robot.request("POST", "/v1/audio/stop", {})
        except Exception as exc:
            audio = {"ok": False, "error": str(exc)}
        return {"ok": True, "motors": motors, "audio": audio}


class MotorHandle:
    """Convenience wrapper for one Vector actuator."""

    def __init__(self, robot: VectorRobot, name: str, motor_id: int):
        self.robot = robot
        self.name = name
        self.id = motor_id

    def state(self) -> dict[str, Any]:
        state = self.robot.motors_state()
        motors = state.get("motors") or state.get("motor") or []
        if len(motors) <= self.id:
            raise VectorRobotError(f"motor {self.id} missing from /v1/motors/state")
        return motors[self.id]

    def power(self, value: float, duration: float = 0.25, ttl_ms: int = 180) -> dict[str, Any]:
        kwargs = {"left": 0.0, "right": 0.0, "lift": 0.0, "head": 0.0}
        kwargs[self.name] = value
        return self.robot.set_motors_for(duration=duration, ttl_ms=ttl_ms, **kwargs)

    def move(self, ticks: int, power: float = 0.4, wait: bool = True, timeout: float = 12.0) -> dict[str, Any]:
        if wait:
            return self.robot.move_motor_and_wait(self.id, ticks, power, timeout)
        return self.robot.move_motor(self.id, ticks, power)

    def precise(self, ticks: int, power: float = 0.25, tolerance: int = 5, timeout: float = 8.0) -> dict[str, Any]:
        return self.robot.move_motor_precise(self.id, ticks, power, tolerance, timeout)

    def hold(self, enabled: bool = True, target: int | None = None, power: float = 0.7, deadband: int = 6) -> dict[str, Any]:
        return self.robot.hold_motor(self.id, enabled, target, power, deadband)

    def release(self) -> dict[str, Any]:
        return self.hold(False)

    def set_continuous(self, power: float):
        """Set continuous power on this specific motor in a background thread."""
        with self.robot._continuous_lock:
            powers = list(self.robot._continuous_powers)
            powers[self.id] = power
        self.robot.set_motors_continuous(powers[0], powers[1], powers[2], powers[3])


class MotorGroup:
    """Ergonomic access to robot actuators: robot.motors.left.move(...)."""

    def __init__(self, robot: VectorRobot):
        self.robot = robot
        self.left = MotorHandle(robot, "left", 0)
        self.right = MotorHandle(robot, "right", 1)
        self.lift = MotorHandle(robot, "lift", 2)
        self.head = MotorHandle(robot, "head", 3)

    def state(self) -> dict[str, Any]:
        return self.robot.motors_state()

    def stop(self) -> dict[str, Any]:
        return self.robot.stop_motors()

    def set(self, left: float = 0, right: float = 0, lift: float = 0, head: float = 0, ttl_ms: int = 250) -> dict[str, Any]:
        return self.robot.set_motors(left, right, lift, head, ttl_ms)

    def for_seconds(
        self,
        left: float = 0,
        right: float = 0,
        lift: float = 0,
        head: float = 0,
        duration: float = 0.25,
    ) -> dict[str, Any]:
        return self.robot.set_motors_for(left, right, lift, head, duration)


class TrackGroup:
    """Track-specific helpers with physical forward-positive tick convention."""

    def __init__(self, robot: VectorRobot):
        self.robot = robot

    def drive(self, ticks: int, power: float = 0.35, timeout: float = 15.0) -> dict[str, Any]:
        return self.robot.drive_distance(ticks, power, timeout)

    def raw(self, left: float, right: float, duration: float) -> dict[str, Any]:
        return self.robot.drive_raw(left, right, duration)

    def forward(self, power: float = 0.25, duration: float = 0.25) -> dict[str, Any]:
        return self.raw(power, power, duration)

    def turn_left(self, power: float = 0.25, duration: float = 0.25) -> dict[str, Any]:
        return self.raw(-power, power, duration)

    def turn_right(self, power: float = 0.25, duration: float = 0.25) -> dict[str, Any]:
        return self.raw(power, -power, duration)

    def set_continuous(self, left: float, right: float):
        """Set continuous powers on left and right tracks in a background thread."""
        with self.robot._continuous_lock:
            lift = self.robot._continuous_powers[2]
            head = self.robot._continuous_powers[3]
        self.robot.set_motors_continuous(left=left, right=right, lift=lift, head=head)
