# Vector Hardware Status

Last updated: 2026-06-01.

This is the operator-facing status page for the minimal Vector hardware image.
Update this file whenever hardware support, API behavior, robot validation, or
known limitations change. The goal is to make project state understandable
without re-reading `vector-hw-api.cpp`, Yocto recipes, or web UI code.

## Reference Sources

Use this workspace as the canonical project root:

```text
/Users/velizard/Projects/wire-os
```

The upstream baseline is:

```text
https://github.com/os-vector/wire-os
```

On 2026-05-22, local `HEAD` matched `origin/main`, but the hardware image and
web UI work were local uncommitted additions. The Anki runtime submodules
`anki/victor`, `anki/wired`, and `anki/vic-cloudless` may be uninitialized in
this checkout. When investigating original behavior, prefer the upstream
submodule source or a fully initialized upstream checkout instead of assuming
the minimal image contains the full original runtime.

Useful local references:

- `anki/rampost/messages.h`: Spine protocol structures.
- `anki/rampost/spine_hal.c`: original low-level Spine framing.
- `anki/rampost/lcd.c`: original LCD GPIO/reset/init/frame path.
- `prebuilt_HY11/apq8009-robot/mm-camera`: Qualcomm/Anki camera binaries.
- `docs/vector-hardware-research.md`: research notes for track encoders,
  power button, hold/precise motion, and BMI160 IMU.
- `docs/vector-high-level-behaviors.md`: roadmap and acceptance criteria for
  calibrated joints, safe hold, DDL animation playback, docking, beamforming,
  and other behavior-level features above the raw hardware API.

## Current Hardware Image

The verified built image is `machine-hw-image`. It keeps APQ8009 BSP,
boot/OTA, Wi-Fi, SSH, `/data`, systemd, selected Qualcomm media packages, and
`vector-hw-api`, but intentionally excludes the Anki personality runtime.

Known included packages from the last exported manifest:

- `vector-hw-api`
- `vector-hw-cli`
- `vector-app-runner`
- `python3-core`
- `python3-json`
- `python3-netclient`
- `python3-modules`
- `openssh`
- `audiohal`
- `alsa-utils`
- `init-audio`
- `adsprpc`
- `mm-camera`
- `rmtstorage`
- `update-engine`
- `update-os`

Known excluded packages:

- `victor`
- `vic-*`
- `wired`
- `anki-robot-target`

## Current API Reality

`vector-hw-api` owns direct hardware access and listens on `0.0.0.0:8080`.
It should remain the single owner of robot hardware devices. Apps and UI code
should use HTTP/WebSocket API calls rather than opening device nodes directly.

Direct devices currently known to the API:

- Spine/body MCU: `/dev/ttyHS0`
- Face LCD SPI: `/dev/spidev1.0`
- IMU SPI presence check: `/dev/spidev0.0`
- Face backlight: `/sys/class/leds/face-backlight*`

Implemented API surface:

- `GET /v1/capabilities` — LLM-readable JSON manifest of all endpoints.
- `GET /v1/status`
- `GET /v1/sensors`
- `GET /v1/motors/state` — current encoder positions, deltas, moving flag for all 4 motors.
- `POST /v1/motors` — raw power open-loop.
- `POST /v1/motors/position` — profiled encoder-based move-by-ticks (background thread per motor).
- `POST /v1/motors/drive` — profiled synchronized straight track move.
- `POST /v1/motors/stop` — cancel position commands, zero all motors.
- `POST /v1/leds/backpack`
- `POST /v1/display/brightness`
- `POST /v1/display/init`
- `POST /v1/display/frame`
- `GET /v1/camera/snapshot` — returns the latest RAM-buffered RGB888-derived BMP frame.
- `GET /v1/camera/stream` — multipart BMP stream from the latest RAM-buffered camera frame.
- `POST /v1/camera/daemon/start` — forks `mm-qcamera-daemon` from `/usr/bin` or `/system/bin`.
- `POST /v1/camera/daemon/stop` — SIGTERM + killall.
- `GET /v1/audio/status`
- `POST /v1/audio/play`
- `POST /v1/audio/stop`
- `POST /v1/audio/volume`
- `POST /v1/audio/stream/start` / `stop` / `GET status`
- App install/start/stop/delete under `/v1/apps`
- `POST /v1/apps/run-script` — trusted Python upload/execute path with streamed stdout/stderr.

Important limitation: several endpoints exist as plumbing or raw adapter
paths, not complete production drivers.

## Hardware Feature Status

| Area | Status | Notes |
| --- | --- | --- |
| Spine MCU | Working adapter | Reads and writes framed Spine messages over `/dev/ttyHS0`. |
| Motors | Raw open-loop + validated profiled encoder position/drive; deployed hold is unsafe under load | API sends raw power for left, right, lift, head. `POST /v1/motors/position` runs per-motor profiled movement using encoder feedback, slew limiting, minimum power, tolerance, and timeout. `POST /v1/motors/drive` starts both track motors together for a synchronized straight encoder move with forward-normalized track feedback. `POST /v1/motors/hold` exists, but the deployed controller oscillates sharply when disturbed and should not be used until rebuilt and revalidated. TTL watchdog still zeros raw motor commands after expiry. Right-track encoder ticks are mirrored relative to positive power, so position/drive/hold control applies a per-motor encoder sign correction. |
| Motor encoders | Driven track direction working; manual track direction is a hardware limitation | `motor[4].position`, `delta`, `time` in Spine telemetry. Exposed via `GET /v1/motors/state`. Web UI shows live positions, deltas, moving indicators. On 2026-06-01 robot `192.168.1.93` was hot-patched to `api_version=0.2.5`: `/v1/motors/drive` with default profile moved `+120` ticks to `left_forward=118`, `right_forward=115`, and `-120` ticks to `left_forward=-111`, `right_forward=-110`; single-motor track `/v1/motors/position` moved left `+80/-80` by `+72/-71` raw ticks and right `+80/-80` by `+51/-70` raw ticks. Research against original Vector/syscon code confirmed the treads use single-channel encoders and syscon signs manual tread deltas from the last driven direction, so hand-rotated treads cannot report reliable bidirectional direction. Distance/angle calibration is still approximate. |
| Backpack LEDs | Solid colors + DDL animation playback working | API writes RGB bytes into the Spine `LightState`. Indexes: 0=back, 1=middle, 2=front, 3=status/button. Since version 0.2.7, LED 3 is automatically inverted (active-low) and the Red/Blue channels are swapped (Channel 9 is physical Blue, Channel 11 is physical Red) at the C++ firmware level to align color intents. Note that the physical Green LED (Channel 10) is ignored/overridden by the body board's power controller, keeping physical Green permanently ON whenever the robot is running. |
| Cliff sensors | Raw telemetry available | `cliffSense[4]` is exposed. Thresholds are not yet calibrated across robots. |
| Battery | Raw telemetry available | Voltage/temp/flags are exposed as raw values. UI currently uses local calibration heuristics. |
| Touch / power button | Partial / under validation | Spine protocol exposes `touchLevel[2]`; observed robot behavior only shows touch 0 working. The physical rear power button is parsed from Spine `PAYLOAD_BOOT_FRAME` / `MicroBodyToHead.buttonPressed` and surfaced as `buttons.power`, `buttons.power_hold_ms`, SDK `power_button_pressed()` / `power_button_hold_ms()`, MCP `power_button_pressed`, and Web UI. `buttons.back` and SDK/MCP `back_button_pressed()` remain as compatibility aliases. |
| Proximity / TOF | Awaiting Robot Validation | Code fix applied, awaiting robot validation. The erratic, physically implausible telemetry (near targets reporting ~8000 mm, laptop-distance jumping to ~62465 mm, and severe discontinuities every 256 mm) was diagnosed as a big-endian to little-endian byte-swapping mismatch: the external VL53L0X sensor registers store values in big-endian, which the Spine STM32 firmware copied raw without swapping. The C++ code now applies `__builtin_bswap16` to all 16-bit proximity telemetry fields inside `bodyJson()`, correcting the values at the source for all clients (Web UI, Python SDK, MCP tools). |
| Display brightness | Basic working if sysfs nodes exist | Writes face backlight sysfs brightness. |
| Display frame | Working | API now initializes LCD GPIO reset/DC, configures `/dev/spidev1.0`, runs Santek/Midas init scripts from `anki/rampost/lcd.c`, and accepts `184x96` RGB565 frames. Previously, the screen would flash/corrupt under load or video streaming because the stock `vic-bootAnim.service` was actively competing for SPI and GPIO lines. Terminating and masking `vic-bootAnim` and `vic-anim` permanently in systemd completely resolved the conflicts. Hot-patch verified with solid RGB565 color patterns on robot `192.168.1.93` on 2026-05-31: all frames rendered perfectly with zero flashing. |
| Camera | RGB snapshots and stream working in daemon hot-patch | API starts or adopts `mm-qcamera-daemon`, starts `mm-anki-camera-wrapper` without `-C`, then uses the original Anki camera IPC protocol: full 144-byte register/start/params/heartbeat messages, SCM_RIGHTS shared-memory fd passing, `CAM0` slot locks, and slot release after copy. It locks all old slots before requesting `RGB888` (`params id=2, format=1`) and unlocks them when the replacement RGB buffer arrives, matching the stock client format-switch barrier. It serves 640x360 24-bit BMP frames and avoids the old RAW10/Bayer decode path and restart-on-stall loop. Validated on robot `192.168.1.89` on 2026-05-25 from a temporary stock-like OS run on port 8081: snapshot returned `image/bmp`, 640x360, 691,254 bytes; sampled RGB channels were not identical; three consecutive snapshots had different MD5s; a 12s stream returned 58 multipart BMP frames, with different middle/end MD5s and ~31% changed sampled pixels. Stock-like OS note: the camera socket is single-client in practice and belongs to group `camera`, so the API now joins that supplementary group when present. |
| Audio output | Basic working on observed robot | API exposes WAV upload playback through `aplay` after running the existing audio mixer init. Verified on robot `192.168.1.89` on 2026-05-22 with a generated 16 kHz mono WAV; `aplay` reported successful playback start. Volume control maps `/v1/audio/volume` to the `RX3 Digital Volume` ALSA mixer (`numid=33`) and was API-validated on robot `192.168.1.89` on 2026-05-23. |
| Microphones | Raw stream working; beamforming suspect | Raw Spine `audio[320]` is exposed. 4-channel interleaved 16kHz signed 16-bit PCM audio can be streamed over UDP to any destination via `/v1/audio/stream/start`. Current beamforming/active-angle UI appears stuck on one angle and needs channel mapping/energy validation. |
| Motion sensors / IMU | Working | User-space Bosch BMI160 IMU integration successfully implemented. The root cause of the previous 0x00 register readings was the BMI160 analog power regulator (8916_l10) being disabled by default, coupled with QUP SPI master controller lockups when probed at low frequencies. Programmatically enabling regulator 8916_l10 in user space and communicating strictly at 15MHz matches original HAL clock constraints and successfully returns CHIP_ID 0xD1. Validated on robot 192.168.1.93 on 2026-05-31: API reports initialized=true and returns scaled accelerometer, gyroscope, and temperature readings. |
| DDL animation playback | Prototype via Web UI/script runner | `tools/vector-web-ui` can index JSON clips and groups from `VECTOR_ANIMATIONS_ROOT`, list them in the `ANIMATIONS` tab, generate a robot-side Python script, and execute it through `/v1/apps/run-script`. Validated on robot `192.168.1.93` on 2026-06-01 with `anim_avs_back2listen_03`, `anim_attention_lookatdevice_01`, and group `ag_vc_laser_lookdown` resolved to `anim_vc_laser_lookdown_01`. Supported prototype tracks are approximate non-blocking head/lift absolute movement, backpack LEDs, rough procedural eyes, profiled straight body motion, and bounded timed body motor power for arcs/turns. Unsupported tracks are reported in stdout. This is not yet a firmware `/v1/animations` API and does not yet render DDL sprite sequences or map Wwise audio events. |
| IR | Unknown | No clear API, DTS, or Spine field found yet. Needs original runtime or hardware investigation. |

## LLM API Goal

The long-term goal of this project is to give an LLM full, clean access to
all robot hardware so it can autonomously control the robot without human
assistance. Key design principles:

- **Self-documenting**: `GET /v1/capabilities` returns a machine-readable JSON
  manifest of all endpoints, parameter types, and notes. An LLM can call this
  once and then understand the full API.
- **Atomic actions**: Each endpoint does one clear thing. The LLM should be able
  to issue `POST /v1/motors/position {motor:2, ticks:1000, power:0.5}` to move
  the lift up 1000 ticks without needing to know about power ramps or TTLs.
- **Rich telemetry**: `GET /v1/sensors` or the SSE stream at `GET /v1/events`
  gives the LLM continuous access to all sensor state (encoders, cliff, touch,
  proximity, IMU, battery).
- **Camera + audio**: The LLM can request a BMP snapshot via
  `GET /v1/camera/snapshot`, receive a PNG-converted snapshot through the MCP
  `camera_snapshot` tool, stream live video via `GET /v1/camera/stream`,
  and play synthesized speech via `POST /v1/audio/play`.

Current client tooling lives in `tools/vector-robot-sdk`: `vector_robot.py`
for Python programs, `vectorctl.py` for CLI use, and `vector_mcp.py` for a
minimal stdio MCP tools server. MCP tools now use SDK-aligned names such as
`get_state`, `drive_raw`, `drive_distance`, `move_joint`, `run_python_async`,
`list_animations`, and `play_animation_async`. The SDK also exposes
object-style helpers such as `robot.motors.lift.move(...)`,
`robot.motors.head.move(...)`, `robot.tracks.forward(...)`, and
`robot.animations.play(...)`. The CLI exposes the same animation path via
`vectorctl animations list/play/stop`. The Web UI now includes a prototype DDL
animation browser/player that proves high-level JSON asset playback can run
through the existing script-runner path. Next steps are packaging the latest
hot-patched `api_version=0.2.5` into the next OTA, moving animation playback
into a stable robot-side app/API, adding calibrated joint commands, encoder
distance/angle calibration, velocity control, sprite/audio asset support, and
fixing TOF/beamforming.

## Practical Roadmap

Recommended order based on current code and likelihood of progress:

1. **Return robot to minimal image and validate RGB camera there.**
   The daemon now uses the original camera IPC protocol and validated RGB888
   frames during a temporary stock-like OS run, but the robot must be switched
   back to the minimal image and hot-patched there before this is considered
   final firmware validation.
2. **Implement calibrated lift/head movement and safe hold.**
   Follow `docs/vector-high-level-behaviors.md` for firmware endpoints,
   SDK/MCP surface, and validation criteria. Animation playback should use
   `height_mm` / `angle_deg` rather than raw ticks.
3. **Implement closed-loop velocity control.**
   Add velocity intents in `vector-hw-api` instead of raw UI motor power.
4. **Harden display streaming.**
   Add frame-rate limits and panel error recovery.
5. **Harden audio playback.**
   Robot validation of volume range, playback state reporting.
6. **Investigate TOF, touch 1, and IR.**
   Treat as uncertain until robot-side tests confirm hardware present.
7. **Harden LLM agent client library.**
   Expand `tools/vector-robot-sdk` with examples, safety limits, and app
   deployment helpers for programs intended to run on the robot.

## Update Rules

When hardware behavior changes, update this file in the same change:

- Change `Last updated`.
- Update the feature table status and notes.
- Add robot validation evidence when available: command run, date, observed
  output, and robot/image build used.
- If an endpoint changes behavior, also update `docs/vector-hw-image.md`.
- If UI behavior changes, also update `docs/vector-web-ui.md`.
- Do not mark a feature as working from code inspection alone. Mark it working
  only after it has been tested on a robot or there is a clear existing runtime
  path already known to work.

## Robot Validation Log

2026-05-22, robot `192.168.1.89`:

- Built `vector-hw` after `cleansstate`; `do_compile`, `do_install`,
  `do_package`, and `machine-hw-image` completed successfully.
- Packaged OTA:
  `/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts/vicos-20260522212342.ota`.
- Added `update-os` to the hardware image and rebuilt/package-tested it so the
  next installed image has the LAN OTA trigger wrapper.
- OTA trigger could not run because the currently installed robot image lacks
  `update-os`, `/anki/bin/update-engine`, and `update-engine.service`.
- Hot-patched `/usr/bin/vector-hw-api` and `/usr/bin/vector-hw-cli` over SSH
  using `~/.ssh/vector/ssh_root_key`; remounted `/` read-write for the copy,
  then remounted read-only and restarted `vector-hw-api.service`.
- `GET /v1/status` after hot-patch reported display `format=rgb565le`,
  `width=184`, `height=96`, and audio `available=true`.
- `POST /v1/display/init` returned `{"ok":true,"panel":"santek"}`.
- `POST /v1/display/frame` with a generated 35,328-byte RGB565 pattern returned
  `{"ok":true,"width":184,"height":96,"format":"rgb565le","panel":"santek"}`.
- `POST /v1/audio/play` with a generated 16 kHz mono WAV returned OK, and
  `/tmp/vector-hw-aplay.log` contained `Playing WAVE`.

2026-05-23, robot `192.168.1.89`:

- Rebuilt only the `vector-hw` Yocto recipe after `cleansstate`; `do_compile`,
  `do_install`, and `do_package` completed successfully.
- Hot-patched `/usr/bin/vector-hw-api` and `/usr/bin/vector-hw-cli` over SSH
  with the updated audio volume endpoint, then restarted
  `vector-hw-api.service`.
- `POST /v1/audio/volume` with `{"level":50}` returned
  `{"ok":true,"volume":50}`.
- Reset volume to `100`; `GET /v1/audio/status` returned
  `{"available":true,"player":"aplay","volume":100}`.

2026-05-24, robot `192.168.1.89`:

- Restored Proximity (TOF) telemetry display in the Web UI, but later user
  validation showed physically implausible values. TOF is now tracked as
  suspect until the original scaling/status path is verified.
- Implemented C++ userspace register SPI polling of the ICM-20608/MPU6500 IMU
  on `/dev/spidev0.0` at 1MHz and exposed scaled telemetry, but observed robot
  logs show effectively constant raw values. IMU is now tracked as suspect.
- Implemented 4-channel UDP audio streaming from raw Spine serial buffers (`audio[320]`) under a custom `"VAUD"` binary protocol over port `5005`.
- Cross-compiled a custom standalone static ARMv7 `vector-wake-word` spotting service using `rustpotter`.
- Implemented dynamic real-time moving-average channel energy selection
  (spatial filtering) to dynamically select the physical microphone receiving
  the cleanest signal. Later UI validation showed the beamforming angle appears
  stuck, so channel mapping/energy calculation still needs validation.
- Spawner thread automatically hits the UDP start endpoint, receives audio packages, normalizing and converting them to `f32`, and runs the keyword spotter. On detection, flashes backpack LEDs in a bright voice-active **cyan** for 1000ms.
- Packed the service in `wake-word-app.tar.gz` and deployed it as an autostart local app via `vector-appctl install`. Verified running and actively selecting channels based on sound source energies.
- Hot-patched freshly rebuilt `vector-hw-api`/`vector-hw-cli` after the Web UI exposed that the running daemon was still `api_version=0.1.0` and lacked `/v1/capabilities`, `/v1/motors/position`, and `/v1/camera/daemon/start`.
- Verified `GET /v1/capabilities` now reports `api_version=0.2.0`.
- Verified `POST /v1/motors/position` on lift with `{"motor":2,"ticks":100,"power":0.4}` returned OK and changed lift encoder position from roughly `23` to `157`.
- Reverse-engineered the original `mm-anki-camera` client path enough for
  snapshots: Unix datagram socket at `/var/run/mm-anki-camera/camera-server`,
  message id `1`, SCM_RIGHTS fd passing, `CAM0` shared-memory header, 6 RAW10
  frame slots at 1280x720 with stride 1600.
- Implemented `GET /v1/camera/snapshot` as an on-demand RAW10 capture path:
  start/detect `mm-qcamera-daemon`, start `mm-anki-camera-wrapper -C`, connect
  to the camera socket, mmap the returned fd, unpack the newest slot, downsample
  to 640x360 grayscale, and return `image/bmp`.
- Validated camera snapshot on robot `192.168.1.89`: `curl
  http://192.168.1.89:8080/v1/camera/snapshot` returned `HTTP/1.1 200 OK`,
  `Content-Type: image/bmp`, `Content-Length: 691254`; the saved frame showed a
  visible room image with raw Bayer/stripe artifacts.
- Added `POST /v1/motors/hold` for robot-side closed-loop encoder hold and
  hot-patch validated it on lift: enable returned
  `{"ok":true,"enabled":true,"motor":2,"target":53,"power":0.7,"deadband":6}`,
  disable returned `{"ok":true,"enabled":false}`, and `/v1/motors/stop` returned OK.
- Added `tools/vector-robot-sdk` with a Python SDK, `vectorctl.py` CLI, and
  `vector_mcp.py` stdio MCP-compatible tools server. Validated `vectorctl.py
  motors` against the robot and `vector_mcp.py` `tools/list` locally.
- Aligned `joystick_control.py` in `tools/vector-robot-sdk` to use the Web UI's
  exact motor ramping logic (ACCEL=0.20, BRAKE=0.35, send_hz=20Hz, and watchdog
  ttl_ms=350ms with a quiet-stop final zero command) for extremely smooth movements.
  Also added complete support for physical head motor hold and nudge buttons (motor index 3)
  analogous to the lift controls, and aligned default scales (drive/turn/lift/head) to `1.0`
  to match Web UI speeds and tank mixing.
- Upgraded the proportional-only (`P-only`) closed-loop encoder hold controller inside `vector-hw-api.cpp`
  to a full closed-loop Proportional-Integral-Derivative (`PID`) controller with anti-windup clamping and deadband friction breakers.
  Later manual disturbance testing showed this controller is too aggressive on the deployed image: the fixed minimum `0.18`
  correction can make lift/head and track recovery overshoot repeatedly. Treat deployed `POST /v1/motors/hold` as unsafe
  until the softer source-side gains are rebuilt and validated.

2026-05-24 (Part 2), robot `192.168.1.89`:

- Fixed the critical Unix datagram socket lifecycle bug: the camera client no
  longer unlinks its bound local path immediately after receiving the shared
  memory fd. `mm-anki-camera` needs that pathname for later `sendto()` calls;
  deleting it makes the server treat the client as dead and stop streaming.
- Added stale `/var/run/mm-anki-camera/camera-server` cleanup when no
  `mm-anki-camera` process owns the socket, preventing repeated
  `ECONNREFUSED` after manual daemon kills.
- Kept the initial shared-memory fd handshake at a 3s receive timeout, then
  switched the mapped camera loop to a short timeout so it can poll frame
  counters even on builds that do not reliably emit frame-ready datagrams.
- Switched default snapshots to 320x180 grayscale BMP generated by 4x4
  LSB-continuous RAW10 averaging and auto-contrast. A comparison against the
  robot's shared-memory dump showed that MIPI RAW10 unpacking is wrong for this
  buffer layout and produces the noisy/broken image. The color Bayer modes
  remain available with `?bayer=...` for diagnostics but are not yet considered
  operator-correct.
- Changed `/v1/camera/stream` to send the latest valid RAM-buffered BMP at the
  target cadence rather than waiting for a new RAW frame before every multipart
  part.
- Updated `tools/vector-web-ui` to consume multipart BMP in JavaScript and draw
  each frame to a canvas. Browsers do not display `multipart/x-mixed-replace`
  BMP streams in an `<img>` the way wire-pod's site displays JPEG streams.
- Checked wire-pod's camera path: it receives stock robot `ImageChunk` data via
  the external interface, decodes it server-side, and re-encodes multipart JPEG
  at quality 50. That explains why its browser stream works, but it does not
  provide a direct replacement for our raw `mm-anki-camera` shared-memory decode.
- Hot-patch rebuilt `vector-hw` after `cleansstate` and deployed
  `/usr/bin/vector-hw-api` and `/usr/bin/vector-hw-cli` over SSH to robot
  `192.168.1.89`.
- Validation on 2026-05-24: `GET /v1/camera/snapshot` returned `200 OK`,
  `image/bmp`, 320x180, 172,854 bytes, with a visible room frame. A 5s
  `GET /v1/camera/stream` request stayed open until client timeout and received
  31 multipart frames, so the stream no longer drops in the first second. The
  web UI at `http://localhost:3111` displayed the stream live in Chrome with
  about 6 FPS.

2026-05-24 (Part 3), robot `192.168.1.89`:

- Reproduced the stale-camera bug: byte-identical snapshots were returned after
  driving the robot because the API served the last RAM BMP even after
  `mm-anki-camera` stopped producing new shared-memory frames.
- Confirmed `mm-anki-camera` sends a first frame-ready message with message id
  `6` and then stops the Qualcomm stream after a short burst. Simple ACK guesses
  did not recover continuous capture; message id `2` repeated events without
  advancing the frame counter, while some other ids destabilized the producer.
- Changed camera reads so snapshots wait up to 2.5s for a new frame sequence
  instead of returning stale cache. The camera thread now restarts
  `mm-anki-camera` after a 1.2s frame-counter stall and reaps exited camera
  child processes to avoid zombie buildup.
- Added `POST /v1/motors/drive` for synchronized straight track movement. MCP
  and `vectorctl.py` now expose `drive_straight`, so LLM clients no longer need
  to issue separate unsynchronized left/right track `move_motor` commands.
- Rebuilt only `vector-hw` after `cleansstate`, hot-patched
  `/usr/bin/vector-hw-api` and `/usr/bin/vector-hw-cli` over SSH, and did not
  build or deploy an OTA.
- Validation: after a clean reboot and hot-patch, consecutive
  `/v1/camera/snapshot` requests returned different valid 172,854-byte BMPs.
  A `drive_straight` 180-tick move followed by a snapshot returned a changed
  view. `vector_mcp.py` listed `drive_straight` and returned two different
  `camera_snapshot` BMP images. Motor state after drive showed all four motors
  `moving=false` with zero deltas. After several snapshots, zombie
  `mm-anki-camera-wrapper` child count was `0`.

2026-05-25, robot `192.168.1.89`:

- Compared the minimal API camera implementation against original `wire-os`
  camera client code and live robot probes. The old daemon path was wrong in
  several important ways: it sent a short registration message instead of the
  full 144-byte `anki_camera_msg`, did not send the regular client heartbeat,
  mmaped the shared buffer read-only, read frame slots without taking/releasing
  the shared-memory locks, confused the packed `bits_per_pixel`/`format` byte
  pair with a RAW10 format value, and recovered by restarting
  `mm-anki-camera` instead of maintaining a valid client session.
- Reworked `vector-hw-api` camera capture to match the original low-level
  protocol: register, start, request `RGB888`, heartbeat every 200 ms, consume
  the fd-delivered `CAM0` shared-memory buffer with `PROT_READ|PROT_WRITE`,
  lock slots using atomic compare/exchange, copy the newest RGB888 frame, then
  release the slot. The API now writes and serves a top-down 24-bit BMP.
- Matched the stock format-change barrier: lock all old shared-memory slots
  before sending the `RGB888` params message, then unlock them before unmapping
  the old buffer when the new RGB buffer fd arrives. Without this, the producer
  could stop advancing frames and force a reconnect after about 5 seconds.
- Changed camera startup to run `mm-anki-camera-wrapper -v 0 -r 1` without
  `-C`; the API owns the capture start message over IPC. `YUV` format was
  tested separately and destabilized `mm-anki-camera`, so the API requests only
  `RGB888`.
- Added zombie-aware process detection for `mm-qcamera-daemon` and
  `mm-anki-camera`, supplementary `camera` group joining when that group exists
  on stock-like systems, and a nonblocking HTTP listener so bind/listen failures
  or SIGTERM do not leave the camera thread running behind a half-started API.
- Hot-patch built `vector-hw` with `cleansstate`. The robot was accidentally
  rebooted into a stock-like OS slot where `wired` owns port 8080 and the
  minimal `vector-hw-api.service` is absent, so validation used the rebuilt
  binary as `/tmp/vector-hw-api --listen 0.0.0.0:8081` without installing it
  permanently. This state must be returned to the minimal image before treating
  robot validation as final for the minimal firmware.
- Updated `tools/vector-web-ui` so the Bun proxy can target a non-default robot
  API port with `VECTOR_ROBOT_PORT`, and changed browser BMP rendering to use
  native `createImageBitmap(image/bmp)` with the previous manual decoder as a
  fallback.
- Updated `tools/vector-robot-sdk/vector_mcp.py` so `camera_snapshot` converts
  robot BMP snapshots to `image/png` MCP image content, while preserving the
  original BMP type/byte count in text metadata. This gives multimodal LLM
  clients a broadly supported image format.
- Validation on 2026-05-25: after stopping the stock camera services so the API
  owned the single camera client, logs showed register/start, initial RAW map
  `6914048`, `RGB888 format requested`, and RGB map `4149248`.
  `GET /v1/camera/snapshot` returned `200 OK`, `image/bmp`, 640x360 24-bit BMP,
  691,254 bytes. Local channel sampling showed RGB means/stdevs differed and
  `channels_identical=false`. A 12s `GET /v1/camera/stream` returned 58
  multipart BMP frames; middle frame 29 and final frame 57 had different MD5s
  and ~31.45% changed sampled pixels. Playwright validation of the Web UI
  camera tab drew 38 canvas frames in 8 seconds, reported `LIVE`, showed
  `640x360` canvas, and had no browser console warnings/errors. MCP validation
  returned `image/png`, 640x360, with original BMP metadata
  (`image/bmp`, 691,254 bytes).

2026-05-25 (Part 2), robot `192.168.1.89` & Switch `192.168.1.74`:

- Diagnosed camera freeze cycle where the Qualcomm `mm-anki-camera` server would stall after exactly 1 second of normal video (4 frames) followed by a 5-second lock waiting for a socket timeout, looping continuously. The issue was traced to a shared-memory buffer slot leak inside `vector-hw-api.cpp` during the format transition.
- Implemented a camera slot leak hotpatch in the robot's `vector-hw-api.cpp` to proactively call `unlockAllSlots();` immediately after the first successful `RGB888` frame is processed. This returns the locked buffer descriptors to the Qualcomm camera producer.
- Rebuilt the `vector-hw` Yocto recipe inside Docker using container `vic-yocto-builder-7` and the case-sensitive build root at `/Volumes/wire-os-cs/wire-os`.
- Deployed the newly compiled binary using the local hotpatch helper command: `/Users/velizard/.codex/skills/wire-os-vector-deploy/scripts/vector_hw_build_deploy.sh hotpatch 192.168.1.89`. This SCPed `vector-hw-api` and `vector-hw-cli` to `/tmp/`, remounted the rootfs read-write, replaced the active `/usr/bin/vector-hw-api` daemon, restored read-only mode, and successfully restarted the service.
- Validated the camera hotpatch on robot `192.168.1.89`: ran a rapid, consecutive loop pulling 20 frame snapshots via HTTP. All 20 frames returned successfully with 0.24s average latency, zero socket timeouts or frame drop stalls, and had 20/20 completely unique MD5 hashes, proving continuous live video capture.
- Compiled the Nintendo Switch Vector Controller homebrew application (`vector-switch-control.nro`) locally via the devkitPro Docker build script `./tools/vector-switch-control/build_switch.sh`.
- Created a robust, generalized Python deployment CLI tool `tools/vector-switch-control/switch_transfer_hub_cli.py` supporting custom target files, IPs, PINs, and cleaning old binary offsets to prevent Transfer Hub offset/overwrite locks.
- Successfully uploaded the newly compiled `vector-switch-control.nro` over Wi-Fi directly to the Nintendo Switch at `192.168.1.74` in under a second using our CLI tool while the Switch Transfer Hub was open on the console.

2026-05-25 (Part 3), robot `192.168.1.89`:

- Configured image inclusion of native Python 3 packages (`python3` + `python3-modules`) inside Yocto and verified a clean, successful `cdbitbake machine-hw-image` build inside Docker.
- Deployed a lightweight, optimized 28MB ARMv7 Python 3 build to `/usr` on the robot's partition under read-write remount, and pre-installed `vector_robot.py` (our SDK) to `/usr/lib/python3.13/vector_robot.py`. Verified that running python3 natively on the robot successfully imports the library with automatic localhost loopback detection.
- Implemented `/v1/apps/run-script` (`POST`) in the C++ `vector-hw-api.cpp` to write uploaded python scripts, fork-exec them unbuffered (`python3 -u`), poll output with a 1-second timeout, stream output chunks in real-time over HTTP, and immediately clean up processes via `SIGKILL` on socket drop. Rebuilt the Yocto recipe and hot-patched the binary on the robot.
- Extended the `vector_robot.py` SDK with `run_script()` stream unchunking and added a `run` subcommand to the PC CLI `vectorctl.py` to upload and stream stdout/stderr prints in real-time. Verified remote script execution and process safety (KeyboardInterrupt and connection drops immediately kill the python process on the robot with zero zombies or leftover temp files).
- Created a gorgeous "DEVELOPER" coding console tab in the browser-based Web UI console using `res.body.getReader()` to load, run, stream, and abort execution with fluid browser console feedbacks.
- Written comprehensive developer guides in `tools/vector-robot-sdk/README.md` covering remote/local scripting APIs, CLI commands, and process lifecycles.

2026-05-25 (Part 4), motor encoder/MCP investigation:

- User MCP test on `motor=1` (`right_track`) reported initial position `-4521`; after `+1000` ticks it observed `-8278`; after `-2000` ticks it eventually stopped around `-4730`. This matches a controller sign bug: right-track positive power decreases encoder position, while the generic single-motor position loop assumed positive power increases ticks for every motor.
- Updated `vector-hw-api.cpp` with a per-motor encoder sign table (`left=+1`, `right=-1`, `lift=+1`, `head=+1`) and applied it to single-motor position moves and motor hold. This is a code fix pending robot hot-patch/build validation.
- Added SDK/MCP `move_motor_and_wait` so LLM clients can run "move then report position" workflows without reading `/v1/motors/state` while the background move is still active.
- Validated raw track encoder signs on robot `192.168.1.89`: a short positive
  left-track raw power command changed left encoder by `+1`, while a short
  positive right-track raw power command changed right encoder by `-6`. Updated
  encoder monitor/TUI displays to show both raw relative ticks and
  forward-normalized track ticks.
- Expanded MCP documentation and tools for agentic control: `robot_help` now
  includes encoder sign conventions and Python SDK guidance,
  `python_control_guide` returns runnable control-loop examples, and
  `run_python` lets an MCP client upload task-specific Python scripts to the
  robot through `/v1/apps/run-script`.
- MCP validation of track encoder-position tools reproduced the bad behavior:
  small `move_motor_and_wait` commands on `right_track` moved the encoder in
  the wrong direction or barely moved, while left-track small moves were only
  approximate. MCP now blocks lift/head-style `move_motor` and
  `move_motor_and_wait` calls for tracks and exposes `drive_for`, a bounded raw
  left/right power command that refreshes TTL and always stops motors.
- Larger raw motor tests showed driven encoder signs are correct in both
  directions: left track `+140/-136`, right track `-57/+154`, lift
  `+172/-182`, and head `+128/-110` for paired forward/backward commands.
  This does not reproduce the user's manual-turn report where a track appeared
  to increase in both directions, so manual low-speed/quadrature behavior still
  needs a hands-on test.
- Tested an SDK/MCP experimental `move_motor_precise` feedback loop. It is not
  reliable enough to call exact on the current robot: lift/head/track moves
  often missed targets by tens of ticks or stalled unless driven with coarse
  raw pulses. MCP marks it experimental and returns `ok=false` when tolerance
  is missed. Stationary `hold_motor` API smoke tests on lift and head held a
  static target for 3 seconds with `max_abs_error=0`, but hold strength under
  external load or after a precise move is not yet validated.
- Added `tools/vector-robot-sdk/motor_lab.py`, a hands-on encoder lab. It can
  move a motor by relative ticks while printing live absolute/raw/normalized
  encoder values, calibrate lift/head lower zero by driving down for a bounded
  time, optionally discover upper range by stall detection, save software
  calibration JSON, and run hold smoke tests. Robot validation with the new lab
  showed track moves are usable with coarse tolerance and sufficient power:
  `left +120/-120` completed with final errors around `+17/-17` using
  `power=0.5`, `min_power=0.35`, `tolerance=20`; `right +120/-120` completed
  with final errors around `-20/+25` using `tolerance=25`. Lift precise move
  with overly high power oscillated and failed, so lift/head exact positioning
  remains a calibration/control task rather than a solved API capability.
- Updated `motor_lab.py` track movement to use pulse-stop-settle-read control
  rather than continuous correction near the target. Track defaults now use
  `power=0.8`, `min_power=0.35`, `pulse=0.25`, `reverse_scale=0.18`, and
  `tolerance=25`, while lift/head keep lower defaults. It now stops after one
  track target crossing by default to avoid repeated forward/back corrections.
  Revalidation with the
  new defaults on `right +120/-120` landed within `+5/+14` ticks without the
  repeated forward/back oscillation.
- User hold disturbance tests on the deployed image showed unsafe active hold:
  head target `-58` wandered from `+28` to `-102` (`max_abs_error=86`), and
  lift target `95` wandered from `11` to `199` (`max_abs_error=104`). MCP
  `hold_motor` and `motor_lab.py hold-test` now refuse active hold by default;
  `motor_lab.py hold-test --unsafe-active` is reserved for guarded firmware
  validation only. Source-side `vector-hw-api.cpp` has been changed to softer
  per-motor gains, lower conditional minimum power, anti-windup on direction
  changes, and output slew limiting, but this still needs a rebuild/deploy
  before robot validation.

2026-05-31, build/deploy attempt:

- Aligned MCP tool names with the Python SDK. `tools/list` now exposes
  `get_state`, `drive_distance`, `drive_raw`, `move_joint`, `stop_motors`,
  `set_backpack_leds`, `camera_snapshot`, `run_python_async`, and
  `get_task_status`; older names remain accepted as compatibility aliases but
  are not advertised.
- Added ergonomic Python SDK helpers:
  `robot.motors.left/right/lift/head`, `robot.lift`, `robot.head`, and
  `robot.tracks`.
- Updated `/v1/apps/run-script` so the daemon writes `/tmp/vector_robot.py`
  before executing an uploaded script. Uploaded scripts can therefore use
  `from vector_robot import VectorRobot` on a clean image.
- Corrected the source-side right-track encoder sign for single-motor
  `/v1/motors/position` and hold control: positive API power moves the right
  track forward while raw right encoder ticks decrease.
- Forced a clean rebuild of `vector-hw` and `machine-hw-image` in Docker.
  `vector-hw:do_compile`, `do_install`, and `do_package` succeeded, then
  `machine-hw-image:do_rootfs`, `do_image_ext4`, and `do_image_complete`
  succeeded.
- Exported build artifacts under
  `/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts`, including
  `machine-hw-image-apq8009-robot.rootfs-20260531091511.manifest`,
  `machine-hw-image-apq8009-robot.rootfs-20260531091511.testdata.json`, and
  `vicos-20260531091618.ota`.
- Verified the new manifest includes `vector-hw-api`, `vector-hw-cli`,
  `vector-app-runner`, `python3-core`, `python3-json`, `python3-netclient`, and
  `python3-modules`, and the checked forbidden runtime package names are still
  absent.
- Attempted OTA deployment to robot `192.168.1.89`; deploy did not run because
  SSH failed with `Operation timed out`/`Host is down`, and `/v1/status` was not
  reachable. A local ping/ARP scan did not find the robot at a replacement IP.
  The 2026-05-31 OTA is built but not installed or robot-validated.
- Original Anki runtime motor PID values were not found in this checkout
  because `anki/victor`, `anki/wired`, and `anki/vic-cloudless` are
  uninitialized submodules. Local `anki/rampost` only exposes the low-level
  Spine protocol, not the high-level motion controller gains.

2026-05-31 (Part 2), robot `192.168.1.93` / `vector.home`:

- Found the robot at `192.168.1.93` after it was not reachable at the older
  `192.168.1.89` address. `GET /v1/status` and SSH were reachable there.
- Deployed `/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts/vicos-20260531091618.ota`
  with `update-os`. The robot downloaded the OTA from
  `http://192.168.1.77:5555/vicos-20260531091618.ota`, progressed to `100%`,
  rebooted, and `vector-hw-api` came back on `192.168.1.93:8080`.
- Post-reboot `GET /v1/status` reported `api_version=0.2.1`, Spine connected,
  camera daemon running, audio available, and motor telemetry valid.
- The deployed `vector-hw-api` binary contains the `/tmp/vector_robot.py`
  script-runner injection, but `/usr/bin/python3` was not present after the OTA
  despite the image manifest listing Python packages. Installed the prepared
  ARMv7 Python runtime tarball
  `/Volumes/wire-os-cs/wire-os/poky/build/python3-armv7.tar.gz` onto `/usr`
  over SSH and added `/usr/bin/python3 -> /usr/bin/python3.13`.
- Verified `/usr/bin/python3 --version` returns `Python 3.13.11` and Python can
  reach `http://127.0.0.1:8080/v1/status` locally on the robot.
- Validated direct script upload:
  `POST /v1/apps/run-script` ran a Python script using
  `from vector_robot import VectorRobot`, printed `api 0.2.1` and
  `motors 4`, and called `robot.stop_motors()`.
- Validated MCP script upload:
  `run_python_async` returned `task_1`; after a short poll,
  `get_task_status` returned `status=completed`, `exit_code=0`, and
  `stdout_stderr="mcp api 0.2.1\n"`.
- Fixed the local deploy helper's trap bug where a successful OTA reboot could
  end with `server_pid: unbound variable`; the helper now stores the HTTP
  server PID in a non-local variable used by the exit trap.

2026-05-31 (Part 3), live telemetry/back-button hot-patch:

- Diagnosed frozen Web UI telemetry and motor control as a stale Spine
  body-frame stream in `vector-hw-api`, not a browser rendering issue. Direct
  `/v1/events`, `/v1/sensors`, and `/v1/motors/state` were repeating the same
  `framecounter`; restarting `vector-hw-api` made telemetry live again.
- Added a serial watchdog in `vector-hw-api` that reopens `/dev/ttyHS0` if no
  body frame arrives for 1500 ms, so a stuck Spine read path can recover
  without manually restarting the service.
- Added parsing for Spine `PAYLOAD_BOOT_FRAME` /
  `MicroBodyToHead.buttonPressed` and exposed the physical back button as
  `buttons.back` / `buttons.back_raw` in `/v1/sensors` and `/v1/status.body`.
  The Python SDK, robot-injected script SDK, MCP, and Web UI now expose the
  same button state.
- Hot-patched robot `192.168.1.93` to `api_version=0.2.2`. Verified
  framecounter increments, `buttons.back` is present, direct head motor command
  changes the encoder, display re-initializes as Santek, and
  `/v1/apps/run-script` can call `VectorRobot.back_button_pressed()`.

2026-05-31 (Part 4), stock service crash UI cleanup:

- After a long power-button hold, the robot rebooted and showed the stock Anki
  / `vic-engine crashed, restarts exhausted` screen. Network/API validation
  showed the minimal hardware daemon was still running (`api_version=0.2.2`);
  the visible error came from the leftover stock `vic-engine.service` failing
  during boot, not from `vector-hw-api`.
- Re-initialized the LCD and sent a black RGB565 frame through
  `/v1/display/init` + `/v1/display/frame`, clearing the stale crash screen.
- Hot-masked `vic-engine.service` on robot `192.168.1.93`, reset failed
  systemd state, and verified `systemctl --failed` returned `0 loaded units`.
- Updated the `vector-hw` recipe to install a `/dev/null` systemd mask for
  `vic-engine.service` in future hardware images, preventing the stock engine
  from grabbing GPIO/Spine or showing crash UI during boot.

2026-05-31 (Part 5), track encoder sign audit:

- Re-checked the original local Anki Spine protocol in `anki/rampost`: motor
  IDs are `MOTOR_LEFT=0`, `MOTOR_RIGHT=1`, `MOTOR_LIFT=2`, `MOTOR_HEAD=3`, and
  `BodyToHead.motor[4]` carries only signed raw `position`, signed raw `delta`,
  and `time`. There is no higher-level direction normalization in `rampost`.
- Confirmed the current API convention remains: positive power moves both
  tracks physically forward, while the mirrored right-track encoder decreases
  in raw ticks. Physical forward-positive track deltas are therefore
  `left_forward = left_raw_delta` and `right_forward = -right_raw_delta`.
- Live short motor tests on robot `192.168.1.93` showed SDK/API-driven encoder
  signs are coherent: left positive power increased raw left ticks, left
  negative power decreased raw left ticks, right positive power decreased raw
  right ticks, and right negative power increased raw right ticks.
- Updated the Web UI encoder panel to display zeroed, physical
  forward-positive `FWD` values for tracks and added `ZERO ENCODER VIEW`; raw
  absolute Spine counters are still available in the value tooltip.
- User follow-up confirmed a remaining limitation: when the tracks are moved by
  hand with motors idle, track encoder counts still move in only one direction.
  Because Linux receives only Body MCU `MotorState.position/delta` and the
  syscon/body firmware is present only as stripped `syscon.dfu` in this
  checkout, manual-direction recovery cannot be fixed in `vector-hw-api` unless
  the Body MCU exposes directional encoder data or its firmware is replaced.
  Added `tools/vector-robot-sdk/manual_encoder_probe.py` to capture raw/manual
  encoder traces for hardware/body-firmware evidence.

2026-05-31 (Part 6), gyroscope calibration & persistence:

- Verified connection to robot `192.168.1.93` and confirmed it runs version `0.2.3`.
- Read live sensor state and validated active telemetry for `accel` (+0.61g, -0.06g, +0.79g) and `gyro` (+0.15dps, +0.12dps, +0.33dps) at rest.
- Executed a 100-sample software zero-rate bias calibration on the robot at rest. Measured biases: `bias_x = 0.160375`, `bias_y = 0.118564`, `bias_z = 0.327158`.
- Persisted calibration data as standard JSON in both `tools/vector-robot-sdk/gyro_calibration.json` locally and `/data/gyro_calibration.json` on the robot's read-write partition via SCP.
- Added comprehensive precision turn integration and re-calibration guides for LLM clients to `docs/vector-mcp.md`.

2026-06-01, profiled track movement hot-patch:

- Studied original Anki motor HAL behavior and confirmed the right tread uses
  mirrored motor direction (`HAL_MOTOR_DIRECTION` left `+1`, right `-1`) while
  track encoder deltas must be forward-normalized for straight movement.
- Updated `vector-hw-api` to `api_version=0.2.5` with profiled
  `/v1/motors/position` and `/v1/motors/drive`: proportional power shaping,
  slew limiting, minimum power, tolerance, timeout, and synchronized left/right
  track error correction. Raw `/v1/motors` remains the joystick/diagnostic API.
- Tuned deployed track defaults to `power=0.45`, `min_power=0.25`, and
  `tolerance=12` after lower `min_power` values stalled near target under load.
- Hot-patched robot `192.168.1.93` and verified `GET /v1/status` reports
  `api_version=0.2.5`. Default `/v1/motors/drive` validation moved `+120`
  ticks to `left_forward=118`, `right_forward=115`, then `-120` ticks to
  `left_forward=-111`, `right_forward=-110`.
- After the robot was recharged and rebooted, re-applied the hot-patch and
  validated the uploaded-script SDK injection: robot-side
  `VectorRobot.drive_straight` exposes `power=0.45`, `min_power`, and
  `tolerance`. A short default 80-tick drive returned `left_forward=77`,
  `right_forward=79`.
- Validated single-track `/v1/motors/position` sign handling: left `+80/-80`
  moved `+72/-71` raw ticks; right `+80/-80` moved `+51/-70` raw ticks.
- Updated SDK defaults and documentation so clients prefer
  `drive_straight()` / `drive_distance()` for animation-like straight track
  movement and reserve raw power for joystick-style control.

2026-06-01, DDL animation playback prototype:

- Added local Web UI endpoints under `tools/vector-web-ui` to index DDL JSON
  clips from `VECTOR_ANIMATIONS_ROOT` and upload a generated Python animation
  player through `/v1/apps/run-script`.
- Verified `GET /local/animations` on `http://localhost:3124` returned
  `1186` clips and `637` groups from `/tmp/vector-animations-build/assets`.
- In the in-app browser, opened the `ANIMATIONS` tab and verified it listed
  clip names, group names, source paths, durations, track counts, candidate
  group clips, and weights.
- Played `anim_avs_back2listen_03` through
  `POST /local/animations/play`; robot `192.168.1.93` returned
  `animation_done` and surfaced unsupported `FaceAnimationKeyFrame`.
- Played `anim_attention_lookatdevice_01` through the same path; robot
  `192.168.1.93` returned `animation_done` and surfaced unsupported
  `RobotAudioKeyFrame`. The test exercised head/lift/body/LED/procedural-face
  scheduling against the current `api_version=0.2.5` script-runner SDK.
- Changed generated animation scripts so head/lift keyframes issue
  non-blocking `/v1/motors/position` commands instead of waiting for each joint
  move to settle; this keeps simultaneous DDL tracks closer to their scheduled
  trigger times. Straight `BodyMotionKeyFrame` commands use `/v1/motors/drive`
  when the computed distance is large enough.
- Played group `ag_vc_laser_lookdown` from the Web UI. The server resolved it
  to `anim_vc_laser_lookdown_01`, the browser streamed the script output, and
  robot `192.168.1.93` returned `animation_done`.
- Added the same DDL animation flow to the Python SDK as
  `robot.animations.list()`, `robot.animations.play(...)`, and
  `robot.animations.stop()`. Local SDK validation played
  `anim_avs_back2listen_03` and returned `SDK_RESULT True`.
- Added MCP tools `list_animations`, `play_animation_async`, and
  `stop_animation`. MCP validation against robot `192.168.1.93` listed the new
  tools, found `ag_vc_laser_lookdown`, and completed
  `play_animation_async` for `anim_avs_back2listen_03` with `exit_code=0`.
- Added CLI commands `vectorctl animations list/play/stop`. CLI validation
  against robot `192.168.1.93` listed `ag_vc_laser_lookdown`, stopped motors
  and audio, and played `ag_vc_laser_lookdown --kind group` to completion with
  `ok=true`.
- This is a high-level prototype only. It should become a robot-side app or
  `/v1/animations` API after calibrated joint movement, sprite rendering, and
  audio event mapping are implemented.

2026-06-01, LED mapping and active-low status correction hot-patch:

- Identified that status LED (index 3) is driven using active-low logic on the Spine hardware interface (common-anode).
- Swapped physical name-to-index mappings in the Python SDK (`tools/vector-robot-sdk/vector_robot.py`) to align with physical layout order: `0=back`, `1=middle`, `2=front`, `3=status/button`.
- Updated C++ daemon (`vector-hw-api.cpp`) to `api_version=0.2.6` with automatic active-low RGB inversion for LED 3, resolving the color mapping discrepancy at the source.
- Rebuilt the Yocto recipe `vector-hw` and successfully hot-patched the active daemon on robot `192.168.1.93`.
- Verified that `GET /v1/status` reports `api_version=0.2.6` and status LED 3 is successfully controlled with exact colors (e.g. RGB `(255, 0, 0)` is Red, `(0, 0, 0)` is Off).
- Corrected the Bun-based Web UI and Python animation player scripts to exclude status LED 3 from the square backpack LED tracks and correctly map DDL back track to LED 0, middle to LED 1, and front to LED 2.

2026-06-01 (Part 2), LED 3 channel swap and physical limitations correction:

- Analysed user color tests to diagnose that LED 3 (status LED) Green channel (Channel 10) is ignored/overridden by the charging/power controller on the body board, keeping the physical Green component permanently ON whenever the robot is running.
- Diagnosed that the Red and Blue channels on LED 3 are physically swapped: Channel 9 controls physical Blue, and Channel 11 controls physical Red under active-low logic.
- Updated C++ daemon (`vector-hw-api.cpp`) to `api_version=0.2.7` to swap indices 9 (physical Blue) and 11 (physical Red) for LED 3, aligning software color intents with physical pins.
- Rebuilt the Yocto recipe `vector-hw` and hot-patched the active daemon on robot `192.168.1.93`.
- Verified `GET /v1/status` reports `api_version=0.2.7` and Spine MCU remains connected.

