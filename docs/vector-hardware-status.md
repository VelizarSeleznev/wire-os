# Vector Hardware Status

Last updated: 2026-05-25.

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

Important limitation: several endpoints exist as plumbing or raw adapter
paths, not complete production drivers.

## Hardware Feature Status

| Area | Status | Notes |
| --- | --- | --- |
| Spine MCU | Working adapter | Reads and writes framed Spine messages over `/dev/ttyHS0`. |
| Motors | Basic open-loop + encoder position/hold + straight drive | API sends raw power for left, right, lift, head. `POST /v1/motors/position` runs per-motor background movement using encoder feedback. `POST /v1/motors/drive` starts both track motors together for a synchronized straight encoder move. `POST /v1/motors/hold` enables a robot-side closed-loop hold target with deadband and power cap. TTL watchdog still zeros raw motor commands after expiry. |
| Motor encoders | Working (read + position move + hold endpoint) | `motor[4].position`, `delta`, `time` in Spine telemetry. Exposed via `GET /v1/motors/state`. Web UI shows live positions, deltas, moving indicators. `POST /v1/motors/position` moves by ticks and was hot-patch validated on robot `192.168.1.89` on 2026-05-24 with a lift move. `POST /v1/motors/hold` was API-validated on lift on 2026-05-24; physical hold strength still needs hands-on tuning. Angle calibration is not done. |
| Backpack LEDs | Basic working | API writes RGB bytes into the Spine `LightState`. |
| Cliff sensors | Raw telemetry available | `cliffSense[4]` is exposed. Thresholds are not yet calibrated across robots. |
| Battery | Raw telemetry available | Voltage/temp/flags are exposed as raw values. UI currently uses local calibration heuristics. |
| Touch sensors | Partial / uncertain | Spine protocol exposes `touchLevel[2]`. Observed robot behavior only shows touch 0 working. Current `touch_hires` in API is not part of the original `rampost` struct and should be treated as experimental until verified. |
| Proximity / TOF | Suspect / needs calibration | Telemetry changes, but observed values are not physically plausible: near objects can report around 8000 mm and laptop-distance targets can report much larger values. Treat `proximity` as raw diagnostic data until the original VL53L0X scaling/status path is verified. |
| Display brightness | Basic working if sysfs nodes exist | Writes face backlight sysfs brightness. |
| Display frame | Basic working on observed robot | API now initializes LCD GPIO reset/DC, configures `/dev/spidev1.0`, runs Santek/Midas init scripts from `anki/rampost/lcd.c`, and accepts `184x96` RGB565 frames. Verified on robot `192.168.1.89` on 2026-05-22 with Santek panel detection and a generated RGB565 test pattern. |
| Camera | RGB snapshots and stream working in daemon hot-patch | API starts or adopts `mm-qcamera-daemon`, starts `mm-anki-camera-wrapper` without `-C`, then uses the original Anki camera IPC protocol: full 144-byte register/start/params/heartbeat messages, SCM_RIGHTS shared-memory fd passing, `CAM0` slot locks, and slot release after copy. It locks all old slots before requesting `RGB888` (`params id=2, format=1`) and unlocks them when the replacement RGB buffer arrives, matching the stock client format-switch barrier. It serves 640x360 24-bit BMP frames and avoids the old RAW10/Bayer decode path and restart-on-stall loop. Validated on robot `192.168.1.89` on 2026-05-25 from a temporary stock-like OS run on port 8081: snapshot returned `image/bmp`, 640x360, 691,254 bytes; sampled RGB channels were not identical; three consecutive snapshots had different MD5s; a 12s stream returned 58 multipart BMP frames, with different middle/end MD5s and ~31% changed sampled pixels. Stock-like OS note: the camera socket is single-client in practice and belongs to group `camera`, so the API now joins that supplementary group when present. |
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
  `GET /v1/camera/snapshot`, receive a PNG-converted snapshot through the MCP
  `camera_snapshot` tool, stream live video via `GET /v1/camera/stream`,
  and play synthesized speech via `POST /v1/audio/play`.

Current client tooling lives in `tools/vector-robot-sdk`: `vector_robot.py`
for Python programs, `vectorctl.py` for CLI use, and `vector_mcp.py` for a
minimal stdio MCP tools server. Next steps are returning the robot to the
minimal image for final camera validation, encoder angle calibration, velocity
control, and fixing TOF/IMU/beamforming.

## Practical Roadmap

Recommended order based on current code and likelihood of progress:

1. **Return robot to minimal image and validate RGB camera there.**
   The daemon now uses the original camera IPC protocol and validated RGB888
   frames during a temporary stock-like OS run, but the robot must be switched
   back to the minimal image and hot-patched there before this is considered
   final firmware validation.
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
- Aligned `joystick_control.py` in `tools/vector-robot-sdk` to use the Web UI's
  exact motor ramping logic (ACCEL=0.20, BRAKE=0.35, send_hz=20Hz, and watchdog
  ttl_ms=350ms with a quiet-stop final zero command) for extremely smooth movements.
  Also added complete support for physical head motor hold and nudge buttons (motor index 3)
  analogous to the lift controls, and aligned default scales (drive/turn/lift/head) to `1.0`
  to match Web UI speeds and tank mixing.
- Upgraded the proportional-only (`P-only`) closed-loop encoder hold controller inside `vector-hw-api.cpp`
  to a full closed-loop Proportional-Integral-Derivative (`PID`) controller with anti-windup clamping and deadband friction breakers.
  Hot-patch rebuilt with cleansstate and deployed over SSH to robot `192.168.1.89`, successfully verifying that the lift now holds targets solid under gravity loads without stall or drift.

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

