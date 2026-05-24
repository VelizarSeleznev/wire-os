#!/usr/bin/env python3
"""Small HTTP client for the minimal WireOS Vector hardware API."""

from __future__ import annotations

import json
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


class VectorRobotError(RuntimeError):
    pass


class VectorRobot:
    def __init__(self, host: str = "192.168.1.89", port: int = 8080, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout

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

    def move_motor(self, motor: int, ticks: int, power: float = 0.5) -> dict[str, Any]:
        return self.request("POST", "/v1/motors/position", {"motor": motor, "ticks": ticks, "power": power})

    def hold_motor(self, motor: int, enabled: bool = True, target: int | None = None, power: float = 0.7, deadband: int = 6) -> dict[str, Any]:
        body: dict[str, Any] = {"motor": motor, "enabled": 1 if enabled else 0}
        if enabled:
            body.update({"power": power, "deadband": deadband})
            if target is not None:
                body["target"] = target
        return self.request("POST", "/v1/motors/hold", body)

    def stop_motors(self) -> dict[str, Any]:
        return self.request("POST", "/v1/motors/stop")

    def set_backpack_leds(self, r: int, g: int, b: int) -> dict[str, Any]:
        return self.request("POST", "/v1/leds/backpack", {"r": r, "g": g, "b": b})

    def display_init(self) -> dict[str, Any]:
        return self.request("POST", "/v1/display/init")

    def camera_snapshot(self, out: str | Path | None = None) -> bytes:
        image = self.request("GET", "/v1/camera/snapshot")
        if not isinstance(image, bytes):
            raise VectorRobotError("camera snapshot returned JSON instead of image bytes")
        if out is not None:
            Path(out).write_bytes(image)
        return image

