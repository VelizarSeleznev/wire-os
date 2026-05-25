# WireOS Vector MCP

`tools/vector-robot-sdk/vector_mcp.py` is a stdio MCP-compatible tool server
for local LLM clients such as LM Studio. It talks to the robot through
`vector-hw-api` on the LAN.

Default robot endpoint:

```text
192.168.1.89:8080
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

Models should call `robot_help` or `motors_state` before issuing movement
commands when they need to know which actuator is which. For straight driving,
prefer `drive_straight` over separate left/right track commands. For lift/head
movement, prefer `move_lift` and `move_head` over raw `move_motor` calls.

## Tools

- `robot_help`: returns the motor map, command guide, and safe-use notes.
- `status`: verifies the API is reachable and returns overall robot status.
- `sensors`: returns raw body sensor telemetry.
- `motors_state`: returns motor names, ids, positions, deltas, and moving flags.
- `move_motor`: moves a raw motor id by relative encoder ticks.
- `drive_straight`: synchronized straight track movement. Positive ticks drive
  forward; negative ticks reverse.
- `move_lift`: alias for `move_motor` with motor id `2`.
- `move_head`: alias for `move_motor` with motor id `3`.
- `hold_motor`: enables closed-loop hold on a motor id.
- `release_motor`: disables closed-loop hold on a motor id.
- `stop_motors`: cancels motion/hold and stops all motors.
- `set_backpack_leds`: sets backpack LEDs with RGB values.
- `camera_snapshot`: captures a camera snapshot, converts the robot's BMP to
  PNG, and returns MCP image content plus text metadata, so multimodal clients
  can put the robot camera frame into the model context directly. The metadata
  includes the original robot MIME type and byte count.

## Validation

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
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"robot_help","arguments":{}}}' \
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
