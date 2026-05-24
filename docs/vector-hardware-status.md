# Vector Hardware Status

Last updated: 2026-05-24.

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

## Current Hardware Image

The verified built image is `machine-hw-image`. It keeps APQ8009 BSP,
boot/OTA, Wi-Fi, SSH, `/data`, systemd, selected Qualcomm media packages, and
`vector-hw-api`, but intentionally excludes the Anki personality runtime.

Known included packages from the last exported manifest:

- `vector-hw-api`
- `vector-hw-cli`
- `vector-app-runner`
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
- `POST /v1/motors/position` — encoder-based move-by-ticks (background thread per motor).
- `POST /v1/motors/stop` — cancel position commands, zero all motors.
- `POST /v1/leds/backpack`
- `POST /v1/display/brightness`
- `POST /v1/display/init`
- `POST /v1/display/frame`
- `GET /v1/camera/snapshot` — reads `/tmp/vector-camera-snapshot.jpg` if present.
- `GET /v1/camera/stream` — MJPEG multipart stream from snapshot file (polls mtime).
- `POST /v1/camera/daemon/start` — forks `mm-qcamera-daemon` from `/usr/bin` or `/system/bin`.
- `POST /v1/camera/daemon/stop` — SIGTERM + killall.
- `GET /v1/audio/status`
- `POST /v1/audio/play`
- `POST /v1/audio/stop`
- `POST /v1/audio/volume`
- `POST /v1/audio/stream/start` / `stop` / `GET status`
- App install/start/stop/delete under `/v1/apps`

Important limitation: several endpoints exist as plumbing or raw adapter
paths, not complete production drivers.

## Hardware Feature Status

| Area | Status | Notes |
| --- | --- | --- |
| Spine MCU | Working adapter | Reads and writes framed Spine messages over `/dev/ttyHS0`. |
| Motors | Basic open-loop + encoder position/hold | API sends raw power for left, right, lift, head. `POST /v1/motors/position` runs per-motor background movement using encoder feedback. `POST /v1/motors/hold` now enables a robot-side closed-loop hold target with deadband and power cap. TTL watchdog still zeros raw motor commands after expiry. |
| Motor encoders | Working (read + position move + hold endpoint) | `motor[4].position`, `delta`, `time` in Spine telemetry. Exposed via `GET /v1/motors/state`. Web UI shows live positions, deltas, moving indicators. `POST /v1/motors/position` moves by ticks and was hot-patch validated on robot `192.168.1.89` on 2026-05-24 with a lift move. `POST /v1/motors/hold` was API-validated on lift on 2026-05-24; physical hold strength still needs hands-on tuning. Angle calibration is not done. |
| Backpack LEDs | Basic working | API writes RGB bytes into the Spine `LightState`. |
| Cliff sensors | Raw telemetry available | `cliffSense[4]` is exposed. Thresholds are not yet calibrated across robots. |
| Battery | Raw telemetry available | Voltage/temp/flags are exposed as raw values. UI currently uses local calibration heuristics. |
| Touch sensors | Partial / uncertain | Spine protocol exposes `touchLevel[2]`. Observed robot behavior only shows touch 0 working. Current `touch_hires` in API is not part of the original `rampost` struct and should be treated as experimental until verified. |
| Proximity / TOF | Suspect / needs calibration | Telemetry changes, but observed values are not physically plausible: near objects can report around 8000 mm and laptop-distance targets can report much larger values. Treat `proximity` as raw diagnostic data until the original VL53L0X scaling/status path is verified. |
| Display brightness | Basic working if sysfs nodes exist | Writes face backlight sysfs brightness. |
| Display frame | Basic working on observed robot | API now initializes LCD GPIO reset/DC, configures `/dev/spidev1.0`, runs Santek/Midas init scripts from `anki/rampost/lcd.c`, and accepts `184x96` RGB565 frames. Verified on robot `192.168.1.89` on 2026-05-22 with Santek panel detection and a generated RGB565 test pattern. |
| Camera | Snapshot working, raw grayscale | API starts/detects `mm-qcamera-daemon` and `mm-anki-camera-wrapper -C`, connects to `/var/run/mm-anki-camera/camera-server`, receives the shared-memory fd, unpacks RAW10 frames, and returns a 640x360 grayscale BMP from `GET /v1/camera/snapshot`. Validated on robot `192.168.1.89` on 2026-05-24. Output is visible but still has raw Bayer/stripe artifacts; color/demosaic and live stream polish remain. |
| Audio output | Basic working on observed robot | API exposes WAV upload playback through `aplay` after running the existing audio mixer init. Verified on robot `192.168.1.89` on 2026-05-22 with a generated 16 kHz mono WAV; `aplay` reported successful playback start. Volume control maps `/v1/audio/volume` to the `RX3 Digital Volume` ALSA mixer (`numid=33`) and was API-validated on robot `192.168.1.89` on 2026-05-23. |
| Microphones | Raw stream working; beamforming suspect | Raw Spine `audio[320]` is exposed. 4-channel interleaved 16kHz signed 16-bit PCM audio can be streamed over UDP to any destination via `/v1/audio/stream/start`. Current beamforming/active-angle UI appears stuck on one angle and needs channel mapping/energy validation. |
| Motion sensors / IMU | Suspect / not trusted | Userspace SPI polling for the MPU6500/ICM-20608 on `/dev/spidev0.0` reads a stable WHO_AM_I, but observed accel/gyro values remain effectively constant (`ax=0 ay=0 az=40 gx=11 gy=-30592 gz=4096` raw pattern in logs). Treat IMU telemetry as untrusted until the register protocol and chip wiring are revalidated. |
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
  `GET /v1/camera/snapshot`, stream live video via `GET /v1/camera/stream`,
  and play synthesized speech via `POST /v1/audio/play`.

Current client tooling lives in `tools/vector-robot-sdk`: `vector_robot.py`
for Python programs, `vectorctl.py` for CLI use, and `vector_mcp.py` for a
minimal stdio MCP tools server. Next steps are encoder angle calibration,
velocity control, camera color/demosaic, and fixing TOF/IMU/beamforming.

## Practical Roadmap

Recommended order based on current code and likelihood of progress:

1. **Harden camera output.**
   Replace the current grayscale RAW10 snapshot with proper Bayer demosaic,
   color correction, and a lower-latency stream path.
2. **Tune motor hold and calibrate encoder ticks.**
   Drive a known distance or angle and record encoder delta to build a
   ticks-per-mm and ticks-per-degree table for all 4 motors. Tune hold gains
   separately for lift/head gravity loads.
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
