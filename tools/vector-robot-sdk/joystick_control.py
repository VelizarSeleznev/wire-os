#!/usr/bin/env python3
"""Joystick control for the minimal WireOS Vector hardware API.

This version avoids pygame joystick events as much as possible because some
Joy-Con / SDL combinations can crash inside pygame.event.get(). Instead it
polls axes and buttons every frame and only uses pygame.event.pump().

Motor handling:
    The joystick loop does NOT directly send HTTP motor commands anymore.
    It updates a shared desired motor state. A separate sender thread refreshes
    robot.set_motors(...) at a steady interval with a longer safety TTL.

Why:
    If every joystick frame performs an HTTP request, the loop can fall behind.
    With short TTL values, Vector moves in short bursts: command -> expires ->
    stop -> next command. The sender thread makes the raw command behave more
    like continuous drive while still keeping the TTL safety watchdog.

Expected project layout:
    vector_robot.py
    joystick_control.py

Run:
    uv run joystick_control.py

Useful probes:
    uv run joystick_control.py --probe
    uv run joystick_control.py --print-inputs
    uv run joystick_control.py --ttl-ms 1500 --send-hz 8
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
import time
from dataclasses import dataclass
from typing import Any

# Must be set before importing pygame.
os.environ.setdefault("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")

import pygame  # noqa: E402

from vector_robot import VectorRobot  # noqa: E402


@dataclass
class ControlConfig:
    host: str
    port: int
    joystick_index: int
    input_hz: float
    send_hz: float
    ttl_ms: int
    deadzone: float
    drive_scale: float
    turn_scale: float
    lift_scale: float
    head_scale: float
    axis_forward: int
    axis_turn: int
    axis_lift: int
    axis_head: int | None
    button_hold: int
    button_release: int
    button_lift_down: int
    button_lift_up: int
    button_stop: int | None
    lift_motor: int
    lift_step_ticks: int
    hold_power: float
    hold_deadband: int
    print_inputs: bool
    button_head_down: int | None
    button_head_up: int | None
    button_head_hold: int | None
    button_head_release: int | None
    head_motor: int
    head_step_ticks: int


@dataclass(frozen=True)
class MotorCommand:
    left: float = 0.0
    right: float = 0.0
    lift: float = 0.0
    head: float = 0.0


class MotorCommandSender:
    """Refresh raw motor power continuously from a background thread."""

    def __init__(self, robot: VectorRobot, cfg: ControlConfig) -> None:
        self.robot = robot
        self.cfg = cfg
        self._lock = threading.Lock()
        self._command = MotorCommand()
        self._current = MotorCommand()
        self._was_moving = False
        self._running = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_send_duration = 0.0
        self._last_error_print = 0.0

    @property
    def last_send_duration(self) -> float:
        return self._last_send_duration

    def start(self) -> None:
        self._running.set()
        self._thread = threading.Thread(target=self._run, name="motor-command-sender", daemon=True)
        self._thread.start()

    def update(self, command: MotorCommand) -> None:
        with self._lock:
            self._command = command

    def stop(self) -> None:
        self._running.clear()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        try:
            self.robot.stop_motors()
        except Exception as exc:  # noqa: BLE001
            print(f"stop_motors failed during shutdown: {exc!r}", file=sys.stderr)

    def _read_command(self) -> MotorCommand:
        with self._lock:
            return self._command

    def _ramp(self, cur: float, tgt: float) -> float:
        if cur == tgt:
            return cur
        # Accel/brake limits matched to Bun server-side motor loop (ACCEL = 0.20, BRAKE = 0.35)
        # Slow down or brake toward zero at a faster rate to ensure responsiveness
        rate = 0.35 if (tgt == 0.0 or abs(tgt) < abs(cur)) else 0.20
        diff = tgt - cur
        sign = 1.0 if diff > 0 else -1.0 if diff < 0 else 0.0
        return cur + sign * min(abs(diff), rate)

    def _run(self) -> None:
        period = 1.0 / self.cfg.send_hz
        ttl_seconds = self.cfg.ttl_ms / 1000.0

        if period >= ttl_seconds * 0.75:
            print(
                f"Warning: send period {period:.3f}s is close to TTL {ttl_seconds:.3f}s. "
                "Increase --ttl-ms or --send-hz if motors pulse.",
                file=sys.stderr,
            )

        while self._running.is_set():
            started = time.monotonic()
            target_cmd = self._read_command()

            # Apply smooth motor ramping per tick
            left_curr = self._ramp(self._current.left, target_cmd.left)
            right_curr = self._ramp(self._current.right, target_cmd.right)
            lift_curr = self._ramp(self._current.lift, target_cmd.lift)
            head_curr = self._ramp(self._current.head, target_cmd.head)

            # Clamp near-zero values
            if abs(left_curr) < 0.005: left_curr = 0.0
            if abs(right_curr) < 0.005: right_curr = 0.0
            if abs(lift_curr) < 0.005: lift_curr = 0.0
            if abs(head_curr) < 0.005: head_curr = 0.0

            self._current = MotorCommand(left=left_curr, right=right_curr, lift=lift_curr, head=head_curr)

            any_moving = (
                self._current.left != 0.0 or
                self._current.right != 0.0 or
                self._current.lift != 0.0 or
                self._current.head != 0.0
            )

            # Only send motor commands when moving or if we were moving in the previous tick
            # (to ensure we send a final zero command so the robot halts instantly)
            if any_moving or self._was_moving:
                try:
                    self.robot.set_motors(
                        self._current.left,
                        self._current.right,
                        self._current.lift,
                        self._current.head,
                        self.cfg.ttl_ms,
                    )
                except Exception as exc:  # noqa: BLE001
                    now = time.monotonic()
                    if now - self._last_error_print > 1.0:
                        print(f"set_motors failed: {exc!r}", file=sys.stderr)
                        self._last_error_print = now
                    time.sleep(0.2)
                    continue

                self._was_moving = any_moving

            self._last_send_duration = time.monotonic() - started

            # If set_motors blocks for a long time, this warning explains bursty motion.
            if self._last_send_duration > ttl_seconds * 0.70:
                now = time.monotonic()
                if now - self._last_error_print > 1.0:
                    print(
                        f"Warning: set_motors took {self._last_send_duration:.3f}s "
                        f"with TTL {ttl_seconds:.3f}s; increase --ttl-ms.",
                        file=sys.stderr,
                    )
                    self._last_error_print = now

            sleep_for = max(0.0, period - self._last_send_duration)
            time.sleep(sleep_for)


class ButtonEdges:
    def __init__(self) -> None:
        self.previous: dict[int, bool] = {}

    def pressed(self, joystick: pygame.joystick.Joystick, button_index: int) -> bool:
        now = get_button(joystick, button_index)
        old = self.previous.get(button_index, False)
        self.previous[button_index] = now
        return now and not old


def clamp(value: float, low: float = -1.0, high: float = 1.0) -> float:
    return max(low, min(high, value))


def apply_deadzone(value: float, deadzone: float) -> float:
    if abs(value) < deadzone:
        return 0.0
    return value


def get_axis(
    joystick: pygame.joystick.Joystick,
    axis_index: int | None,
    *,
    deadzone: float,
    invert: bool = False,
) -> float:
    if axis_index is None:
        return 0.0
    if axis_index < 0 or axis_index >= joystick.get_numaxes():
        return 0.0
    value = float(joystick.get_axis(axis_index))
    if invert:
        value = -value
    return apply_deadzone(value, deadzone)


def get_button(joystick: pygame.joystick.Joystick, button_index: int | None) -> bool:
    if button_index is None:
        return False
    if button_index < 0 or button_index >= joystick.get_numbuttons():
        return False
    return bool(joystick.get_button(button_index))


def safe_event_pump() -> bool:
    """Pump pygame events without using pygame.event.get()."""

    try:
        pygame.event.pump()
        return True
    except (KeyError, SystemError, pygame.error) as exc:
        print(f"pygame event pump failed: {exc!r}", file=sys.stderr)
        return False


def block_joystick_events() -> None:
    """Prevent pygame from filling the queue with joystick events.

    State polling still works after pygame.event.pump().
    """

    joystick_event_types = [
        pygame.JOYAXISMOTION,
        pygame.JOYBALLMOTION,
        pygame.JOYHATMOTION,
        pygame.JOYBUTTONDOWN,
        pygame.JOYBUTTONUP,
        pygame.JOYDEVICEADDED,
        pygame.JOYDEVICEREMOVED,
    ]
    for event_type in joystick_event_types:
        try:
            pygame.event.set_blocked(event_type)
        except pygame.error:
            pass


def init_pygame() -> None:
    pygame.init()
    pygame.joystick.init()

    # A tiny hidden window gives SDL a video/event context on some platforms.
    try:
        pygame.display.set_mode((1, 1), flags=pygame.HIDDEN)
    except pygame.error:
        # Headless-ish fallback. Joystick polling can still work on some systems.
        pass

    block_joystick_events()


def open_joystick(index: int) -> pygame.joystick.Joystick:
    count = pygame.joystick.get_count()
    if count <= 0:
        raise RuntimeError("No joystick detected by pygame.")
    if index < 0 or index >= count:
        raise RuntimeError(f"Joystick index {index} is invalid; detected {count} joystick(s).")

    joystick = pygame.joystick.Joystick(index)
    joystick.init()
    return joystick


def print_joystick_info(joystick: pygame.joystick.Joystick) -> None:
    print(f"Using joystick: {joystick.get_name()}")
    print(f"  instance id: {joystick.get_instance_id()}")
    print(f"  axes:        {joystick.get_numaxes()}")
    print(f"  buttons:     {joystick.get_numbuttons()}")
    print(f"  hats:        {joystick.get_numhats()}")


def probe_joysticks() -> None:
    init_pygame()
    count = pygame.joystick.get_count()
    print(f"joystick count: {count}")
    for index in range(count):
        joystick = pygame.joystick.Joystick(index)
        joystick.init()
        print()
        print(f"index:       {index}")
        print(f"instance id: {joystick.get_instance_id()}")
        print(f"name:        {joystick.get_name()}")
        print(f"axes:        {joystick.get_numaxes()}")
        print(f"buttons:     {joystick.get_numbuttons()}")
        print(f"hats:        {joystick.get_numhats()}")
    pygame.quit()


def extract_motor_position(motors_state: Any, motor_index: int) -> int | None:
    """Try a few common JSON shapes and return a motor position/tick value.

    This keeps joystick_control.py independent of the exact hardware API shape.
    Use `uv run vectorctl motors` to verify the real response if this returns None.
    """

    candidates: list[Any] = []

    if isinstance(motors_state, dict):
        if "motors" in motors_state:
            candidates.append(motors_state["motors"])
        if "data" in motors_state:
            candidates.append(motors_state["data"])
        candidates.append(motors_state)
    else:
        candidates.append(motors_state)

    for item in candidates:
        # Shape: {"motors": [{"position": 123}, ...]}
        if isinstance(item, list) and 0 <= motor_index < len(item):
            motor = item[motor_index]
            if isinstance(motor, dict):
                for key in ("position", "pos", "ticks", "encoder", "encoder_ticks"):
                    if key in motor:
                        return int(motor[key])
            elif isinstance(motor, (int, float)):
                return int(motor)

        # Shape: {"2": {"position": 123}}
        if isinstance(item, dict):
            for key in (motor_index, str(motor_index), f"motor{motor_index}"):
                if key in item:
                    motor = item[key]
                    if isinstance(motor, dict):
                        for pos_key in ("position", "pos", "ticks", "encoder", "encoder_ticks"):
                            if pos_key in motor:
                                return int(motor[pos_key])
                    elif isinstance(motor, (int, float)):
                        return int(motor)

    return None


def read_motor_position(robot: VectorRobot, motor_index: int) -> int | None:
    try:
        state = robot.motors_state()
    except Exception as exc:  # noqa: BLE001 - robot client can raise urllib/runtime exceptions
        print(f"Could not read motor state: {exc!r}", file=sys.stderr)
        return None
    return extract_motor_position(state, motor_index)


def hold_motor_at_current_position(robot: VectorRobot, motor_index: int, cfg: ControlConfig, label: str) -> None:
    position = read_motor_position(robot, motor_index)
    if position is None:
        print(f"Could not find {label} position in motors_state(); hold ignored.", file=sys.stderr)
        return
    result = robot.hold_motor(motor_index, True, position, cfg.hold_power, cfg.hold_deadband)
    print(f"Holding {label} at {position} ticks: {result}")


def nudge_motor_target(robot: VectorRobot, motor_index: int, cfg: ControlConfig, delta_ticks: int, label: str) -> None:
    position = read_motor_position(robot, motor_index)
    if position is None:
        print(f"Could not find {label} position in motors_state(); nudge ignored.", file=sys.stderr)
        return
    target = position + delta_ticks
    result = robot.hold_motor(motor_index, True, target, cfg.hold_power, cfg.hold_deadband)
    print(f"[{label}] target {target} ticks: {result}")


def release_motor_hold(robot: VectorRobot, motor_index: int, label: str) -> None:
    result = robot.hold_motor(motor_index, False)
    print(f"{label} hold released: {result}")


def print_controls(cfg: ControlConfig) -> None:
    print()
    print("Controls:")
    print(f"  axis {cfg.axis_forward:<2} = forward/back")
    print(f"  axis {cfg.axis_turn:<2} = turn")
    print(f"  axis {cfg.axis_lift:<2} = manual lift")
    if cfg.axis_head is not None:
        print(f"  axis {cfg.axis_head:<2} = manual head")
    print(f"  button {cfg.button_hold:<2} = hold current lift position")
    print(f"  button {cfg.button_release:<2} = disable lift hold")
    print(f"  button {cfg.button_lift_down:<2} = lift target -{cfg.lift_step_ticks} ticks")
    print(f"  button {cfg.button_lift_up:<2} = lift target +{cfg.lift_step_ticks} ticks")
    if cfg.button_head_hold is not None:
        print(f"  button {cfg.button_head_hold:<2} = hold current head position")
    if cfg.button_head_release is not None:
        print(f"  button {cfg.button_head_release:<2} = disable head hold")
    if cfg.button_head_down is not None:
        print(f"  button {cfg.button_head_down:<2} = head target -{cfg.head_step_ticks} ticks")
    if cfg.button_head_up is not None:
        print(f"  button {cfg.button_head_up:<2} = head target +{cfg.head_step_ticks} ticks")
    if cfg.button_stop is not None:
        print(f"  button {cfg.button_stop:<2} = emergency stop motors")
    print("  Ctrl+C    = stop")
    print()
    print("Motor refresh:")
    print(f"  send_hz: {cfg.send_hz:g} Hz")
    print(f"  ttl_ms:  {cfg.ttl_ms} ms")
    print()


def run_control_loop(cfg: ControlConfig) -> int:
    init_pygame()
    joystick = open_joystick(cfg.joystick_index)
    print_joystick_info(joystick)
    print_controls(cfg)

    robot = VectorRobot(cfg.host, cfg.port)
    sender = MotorCommandSender(robot, cfg)
    sender.start()

    edges = ButtonEdges()
    period = 1.0 / cfg.input_hz
    running = True

    def request_stop(signum: int, frame: object) -> None:  # noqa: ARG001
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    try:
        while running:
            loop_started = time.monotonic()

            if not safe_event_pump():
                sender.update(MotorCommand())
                time.sleep(0.25)
                continue

            forward = get_axis(joystick, cfg.axis_forward, deadzone=cfg.deadzone, invert=True)
            turn = get_axis(joystick, cfg.axis_turn, deadzone=cfg.deadzone)
            manual_lift = get_axis(joystick, cfg.axis_lift, deadzone=cfg.deadzone, invert=True)
            manual_head = get_axis(joystick, cfg.axis_head, deadzone=cfg.deadzone, invert=True)

            left = clamp((forward * cfg.drive_scale) + (turn * cfg.turn_scale))
            right = clamp((forward * cfg.drive_scale) - (turn * cfg.turn_scale))
            lift = clamp(manual_lift * cfg.lift_scale)
            head = clamp(manual_head * cfg.head_scale)

            command = MotorCommand(left=left, right=right, lift=lift, head=head)
            sender.update(command)

            if cfg.print_inputs:
                axes = [round(float(joystick.get_axis(i)), 3) for i in range(joystick.get_numaxes())]
                buttons = [int(joystick.get_button(i)) for i in range(joystick.get_numbuttons())]
                print(
                    f"axes={axes} buttons={buttons} -> "
                    f"left={left:.2f} right={right:.2f} lift={lift:.2f} head={head:.2f} "
                    f"http={sender.last_send_duration * 1000:.0f}ms",
                    end="\r",
                    flush=True,
                )

            # Lift holds and nudges
            if edges.pressed(joystick, cfg.button_hold):
                sender.update(MotorCommand())
                hold_motor_at_current_position(robot, cfg.lift_motor, cfg, "Lift")

            if edges.pressed(joystick, cfg.button_release):
                release_motor_hold(robot, cfg.lift_motor, "Lift")

            if edges.pressed(joystick, cfg.button_lift_down):
                sender.update(MotorCommand())
                nudge_motor_target(robot, cfg.lift_motor, cfg, -cfg.lift_step_ticks, "Lift")

            if edges.pressed(joystick, cfg.button_lift_up):
                sender.update(MotorCommand())
                nudge_motor_target(robot, cfg.lift_motor, cfg, cfg.lift_step_ticks, "Lift")

            # Head holds and nudges
            if cfg.button_head_hold is not None and edges.pressed(joystick, cfg.button_head_hold):
                sender.update(MotorCommand())
                hold_motor_at_current_position(robot, cfg.head_motor, cfg, "Head")

            if cfg.button_head_release is not None and edges.pressed(joystick, cfg.button_head_release):
                release_motor_hold(robot, cfg.head_motor, "Head")

            if cfg.button_head_down is not None and edges.pressed(joystick, cfg.button_head_down):
                sender.update(MotorCommand())
                nudge_motor_target(robot, cfg.head_motor, cfg, -cfg.head_step_ticks, "Head")

            if cfg.button_head_up is not None and edges.pressed(joystick, cfg.button_head_up):
                sender.update(MotorCommand())
                nudge_motor_target(robot, cfg.head_motor, cfg, cfg.head_step_ticks, "Head")

            if cfg.button_stop is not None and edges.pressed(joystick, cfg.button_stop):
                print("Emergency stop button pressed.")
                sender.update(MotorCommand())
                robot.stop_motors()
                continue

            elapsed = time.monotonic() - loop_started
            sleep_for = max(0.0, period - elapsed)
            time.sleep(sleep_for)

    finally:
        if cfg.print_inputs:
            print()
        sender.update(MotorCommand())
        sender.stop()
        print("Stopped motors.")
        pygame.quit()

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.1.89")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--joystick", type=int, default=0, help="pygame joystick index")
    parser.add_argument("--probe", action="store_true", help="list joysticks and exit")
    parser.add_argument("--print-inputs", action="store_true", help="print live axes/buttons")

    parser.add_argument("--input-hz", type=float, default=30.0, help="joystick polling rate")
    parser.add_argument("--send-hz", type=float, default=20.0, help="HTTP motor refresh rate")
    parser.add_argument("--ttl-ms", type=int, default=350, help="motor watchdog TTL sent to robot")
    parser.add_argument("--deadzone", type=float, default=0.08)

    parser.add_argument("--drive-scale", type=float, default=1.0)
    parser.add_argument("--turn-scale", type=float, default=1.0)
    parser.add_argument("--lift-scale", type=float, default=1.0)
    parser.add_argument("--head-scale", type=float, default=1.0)

    parser.add_argument("--axis-forward", type=int, default=1)
    parser.add_argument("--axis-turn", type=int, default=0)
    parser.add_argument("--axis-lift", type=int, default=3)
    parser.add_argument("--axis-head", type=int, default=None)

    parser.add_argument("--button-hold", type=int, default=0)
    parser.add_argument("--button-release", type=int, default=1)
    parser.add_argument("--button-lift-down", type=int, default=4)
    parser.add_argument("--button-lift-up", type=int, default=5)
    
    parser.add_argument("--button-head-hold", type=int, default=None)
    parser.add_argument("--button-head-release", type=int, default=None)
    parser.add_argument("--button-head-down", type=int, default=None)
    parser.add_argument("--button-head-up", type=int, default=None)
    
    parser.add_argument("--button-stop", type=int, default=None)

    parser.add_argument("--lift-motor", type=int, default=2)
    parser.add_argument("--lift-step-ticks", type=int, default=50)
    parser.add_argument("--head-motor", type=int, default=3)
    parser.add_argument("--head-step-ticks", type=int, default=30)
    
    parser.add_argument("--hold-power", type=float, default=0.7)
    parser.add_argument("--hold-deadband", type=int, default=6)
    return parser


def config_from_args(args: argparse.Namespace) -> ControlConfig:
    return ControlConfig(
        host=args.host,
        port=args.port,
        joystick_index=args.joystick,
        input_hz=args.input_hz,
        send_hz=args.send_hz,
        ttl_ms=args.ttl_ms,
        deadzone=args.deadzone,
        drive_scale=args.drive_scale,
        turn_scale=args.turn_scale,
        lift_scale=args.lift_scale,
        head_scale=args.head_scale,
        axis_forward=args.axis_forward,
        axis_turn=args.axis_turn,
        axis_lift=args.axis_lift,
        axis_head=args.axis_head,
        button_hold=args.button_hold,
        button_release=args.button_release,
        button_lift_down=args.button_lift_down,
        button_lift_up=args.button_lift_up,
        button_stop=args.button_stop,
        lift_motor=args.lift_motor,
        lift_step_ticks=args.lift_step_ticks,
        hold_power=args.hold_power,
        hold_deadband=args.hold_deadband,
        print_inputs=args.print_inputs,
        button_head_down=args.button_head_down,
        button_head_up=args.button_head_up,
        button_head_hold=args.button_head_hold,
        button_head_release=args.button_head_release,
        head_motor=args.head_motor,
        head_step_ticks=args.head_step_ticks,
    )


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.probe:
        probe_joysticks()
        return 0

    cfg = config_from_args(args)
    return run_control_loop(cfg)


if __name__ == "__main__":
    raise SystemExit(main())
