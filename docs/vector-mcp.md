# WireOS Vector MCP

`tools/vector-robot-sdk/vector_mcp.py` is a stdio MCP-compatible tool server
for local LLM clients such as LM Studio. It talks to the robot through
`vector-hw-api` on the LAN.

Preferred runtime: run the MCP implementation on `seggver` through the managed
SSH wrapper installed by `scripts/seggver-runtime.sh`:

```text
/Users/velizard/bin/wireos-vector-mcp
```

That local command is only a stdio SSH pipe. The Python SDK and robot network
calls run on `egg@seggver:/home/egg/wire-os-runtime/source/tools/vector-robot-sdk`.
This avoids laptop port/process drift while preserving compatibility with MCP
clients that launch stdio commands.

Default robot endpoint:

```text
vector.home:8080
```

## Tool Model Contract

The MCP tool descriptions intentionally include the motor map and safety notes
so a model does not need to infer hardware indexes from context:

```text
0 = left_track
1 = right_track
2 = lift
3 = head
```

Track encoder signs are intentionally documented in MCP help. Raw encoder ticks
are mirrored between tracks: left-track forward motion usually increases raw
ticks, while right-track forward motion usually decreases raw ticks. For
physical forward-positive track deltas, use:

```text
left_forward_delta = left_raw_delta
right_forward_delta = -right_raw_delta
```

Models should call `get_state` before issuing movement commands when they need
to know current actuator positions. MCP tool names intentionally mirror the
Python SDK method names where practical. For track movement, prefer
`drive_raw` for short time-boxed moves or `drive_distance` when encoder
feedback is wanted. For lift/head movement, use `move_joint`.

## Tools

- `set_robot_ip`: dynamically changes the robot's target IP address and saves it persistently to `mcp_config.json`.
- `get_state`: SDK-aligned state snapshot (`VectorRobot.get_state()`).
- `drive_distance`: SDK-aligned straight encoder drive
  (`VectorRobot.drive_distance()`).
- `drive_raw`: SDK-aligned bounded raw track drive
  (`VectorRobot.drive_raw()`).
- `move_joint`: SDK-aligned lift/head movement (`VectorRobot.move_joint()`).
- `stop_motors`: cancels motion/hold and stops all motors.
- `power_button_pressed`: SDK-aligned physical rear power button state and hold
  duration (`VectorRobot.power_button_pressed()` /
  `VectorRobot.power_button_hold_ms()`).
- `back_button_pressed`: compatibility alias for the same physical rear power
  button (`VectorRobot.back_button_pressed()`).
- `set_backpack_leds`: sets all backpack LEDs to one RGB color.
- `set_backpack_led`: sets one backpack LED by index while leaving the other
  LEDs unchanged.
- `camera_snapshot`: captures a camera snapshot, converts the robot's BMP to
  PNG, and returns MCP image content plus text metadata, so multimodal clients
  can put the robot camera frame into the model context directly. The metadata
  includes the original robot MIME type and byte count.
- `list_animations`: SDK-aligned `robot.animations.list()` wrapper for DDL
  `vector-animations-build` clips and animation groups available from
  `VECTOR_ANIMATIONS_ROOT` on the MCP host.
- `play_animation_async`: SDK-aligned `robot.animations.play()` wrapper. It
  plays a DDL clip or group asynchronously through `/v1/apps/run-script` and
  returns a `task_id`; use `get_task_status` to read output and completion.
- `stop_animation`: stops motors and audio for the active animation prototype.
- `run_python_async`: uploads a complete Python script to the robot, starts it
  in the background, and returns `task_id` immediately. Scripts can import
  `VectorRobot` with `from vector_robot import VectorRobot`; the firmware writes
  a local SDK module beside the uploaded script before execution.
- `get_task_status`: polls a `run_python_async` task and returns newly buffered
  stdout/stderr plus task status.

Older tool names such as `status`, `sensors`, `motors_state`, `drive_for`,
`move_lift`, and `move_head` are still accepted by the server as compatibility
aliases, but they are intentionally omitted from `tools/list`.

## Python Control Pattern

Scripts run through `run_python_async` should import `VectorRobot`, wrap
movement in `try/finally`, and stop motors in cleanup:

```python
from vector_robot import VectorRobot
import time

robot = VectorRobot()
try:
    robot.set_motors(left=0.2, right=0.2, ttl_ms=200)
    time.sleep(0.2)
finally:
    robot.stop_motors()
```

### Gyroscope Calibration & Precision Turns

The robot's 6-axis IMU (Bosch BMI160) registers raw angular velocities in degrees per second (`deg/s`). Any constant offset at rest is the gyroscope's zero-rate bias. To ensure precise rotational movements (e.g., exactly 90 degrees), models must load and apply the calibration bias offsets.

#### Deployed Calibration
A successful 100-sample calibration was performed on `2026-05-31` with the robot at rest. The resulting calibration is persisted on the robot at `/data/gyro_calibration.json` and in the SDK directory at `tools/vector-robot-sdk/gyro_calibration.json`:

```json
{
    "bias_x": 0.160374855,
    "bias_y": 0.11856443,
    "bias_z": 0.32715845
}
```

#### How to use in Scripts
To perform a precision turn, subtract `bias_z` from the raw `gyro.z` reading to obtain the clean angular velocity (`gz_clean`), and numerically integrate it over time:

```python
from vector_robot import VectorRobot
import time
import json

robot = VectorRobot()

# Load saved calibration values (fallback to defaults if missing)
try:
    with open("/data/gyro_calibration.json", "r") as f:
        calib = json.load(f)
except Exception:
    calib = {"bias_z": 0.32715845}

bias_z = calib.get("bias_z", 0.32715845)

try:
    # Start turning left
    robot.set_motors(left=-0.3, right=0.3)

    current_angle = 0.0
    last_time = time.monotonic()

    # Keep turning until the integrated angle reaches 90 degrees
    while abs(current_angle) < 90.0:
        time.sleep(0.01)  # fast poll loop
        now = time.monotonic()
        dt = now - last_time
        last_time = now

        state = robot.sensors()
        if "gyro" in state:
            gz_clean = state["gyro"]["z"] - bias_z
            current_angle += gz_clean * dt
finally:
    robot.stop_motors()
```

#### Redoing Calibration
If the robot's environment temperature changes significantly or you observe rotational drift during stationary tasks, run a new 2-second calibration using `run_python_async` to overwrite the configuration:

```python
from vector_robot import VectorRobot
import time
import json

robot = VectorRobot()
gx_sum = gy_sum = gz_sum = 0
valid = 0

for _ in range(100):
    state = robot.sensors()
    if "gyro" in state:
        gx_sum += state["gyro"]["x"]
        gy_sum += state["gyro"]["y"]
        gz_sum += state["gyro"]["z"]
        valid += 1
    time.sleep(0.02)

if valid > 0:
    calib = {
        "bias_x": gx_sum / valid,
        "bias_y": gy_sum / valid,
        "bias_z": gz_sum / valid
    }
    with open("/data/gyro_calibration.json", "w") as f:
        json.dump(calib, f, indent=4)
    print("Calibration re-calculated and saved to /data/gyro_calibration.json")
```

## Validation

Preferred server-backed validation:

```sh
scripts/seggver-runtime.sh mcp-test
```

This should list tools even when the robot is offline. Calls that require robot
hardware may fail while Vector is off, asleep, discharged, rebooting, or on a
different DHCP lease; treat that as robot availability, not MCP runtime failure.

List tools:

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"manual","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  | /usr/bin/python3 tools/vector-robot-sdk/vector_mcp.py --host 192.168.1.89 --port 8080
```

Read the robot guide:

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"manual","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_state","arguments":{}}}' \
  | /usr/bin/python3 tools/vector-robot-sdk/vector_mcp.py --host 192.168.1.89 --port 8080
```

Capture a camera frame for an LLM:

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"manual","version":"0"}}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"camera_snapshot","arguments":{}}}' \
  | /usr/bin/python3 tools/vector-robot-sdk/vector_mcp.py --host 192.168.1.89 --port 8080
```

2026-05-25 validation against a temporary API run on port `8081` returned MCP
image content with `mimeType=image/png`, 640x360 RGB PNG data, and metadata
showing the original robot snapshot was `image/bmp`, 691,254 bytes.

2026-05-31 local validation: `tools/list` returned exactly the SDK-aligned
canonical tool names `get_state`, `drive_distance`, `drive_raw`, `move_joint`,
`stop_motors`, `set_backpack_leds`, `camera_snapshot`, `run_python_async`, and
`get_task_status`. After back-button support, `back_button_pressed` was added
as the SDK-aligned button helper. Robot-side validation could not run because
`192.168.1.89` was unreachable over HTTP and SSH.

2026-05-31 validation against robot `192.168.1.93`: `tools/list` includes
`back_button_pressed`, and calling it returned `{"pressed": false}` while the
physical rear button was not pressed.

2026-06-01 validation against robot `192.168.1.93`:

- `tools/list` includes `list_animations`, `play_animation_async`, and
  `stop_animation`.
- `list_animations` with filter `ag_vc_laser_lookdown` returned
  `total_clips=1186`, `total_groups=637`, and the group pointing to
  `anim_vc_laser_lookdown_01`.
- `play_animation_async` for `anim_avs_back2listen_03` returned `task_1`; after
  polling `get_task_status`, the task completed with `exit_code=0` and output
  containing `animation_done anim_avs_back2listen_03`.
