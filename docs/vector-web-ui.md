# Vector Web UI

Canonical source:

```text
tools/vector-web-ui
```

The older standalone folder `/Users/velizard/Projects/vector-web-ui` was copied
into this repository so firmware and control UI work live together. New changes
should be made in `tools/vector-web-ui`.

## Purpose

The web UI is a local LAN control console for the minimal Vector hardware API.
It talks to `vector-hw-api` on the robot through a Bun proxy server:

```text
browser <-> Bun server on localhost:3000 <-> robot vector-hw-api on :8080
```

Current firmware/hardware capability status is tracked in:

```text
docs/vector-hardware-status.md
```

The Bun server owns the high-rate command loop for motors. The browser sends
target motor values over WebSocket, and the server ramps/sends motor commands
to the robot every 50 ms with `ttl_ms: 350`.

## Run

```sh
cd tools/vector-web-ui
VECTOR_ROBOT_IP=<robot-ip> bun run dev
```

Default URL:

```text
http://localhost:3000
```

Environment:

- `PORT`: UI/proxy server port, default `3000`.
- `VECTOR_ROBOT_IP`: default robot IP shown to clients and used by the proxy.

## Implemented UI Surface

- Connect/disconnect WebSocket to local Bun proxy.
- Drive joystick with server-side motor ramping.
- Head and lift sliders.
- Backpack LED color presets.
- Face backlight brightness with direct HTTP slider updates.
- Face display image upload and one-frame send.
- Face display video preview and repeated RGB565 frame streaming.
- Browser-side audio decode/resample to 16 kHz mono WAV, client-side volume
  gain, and audio stop controls.
- Telemetry relay from robot `/v1/events` SSE stream.
- Battery, cliff, touch, frame-counter display.
- Real-time 16 kHz mono microphone audio streaming from robot to browser via UDP 5005 relay with low-latency Web Audio queuing.
- Circular beamforming sound visualizer showing dynamic RMS energies for all 4 microphones and pointer arrow for active sound-source direction.
- Browser-side real-time WAV recording and download of the beamformed microphone stream.
- Live 6-axis IMU (motion sensor) accelerometer (g) and gyroscope (deg/s) telemetry readouts.
- Sleek glassmorphic Camera Feed placeholder card with CSS scanline animation.
- Local event log.

## Current Fixes After Import

The imported copy was adjusted so it is easier to reuse from this project:

- Default robot IP can be set with `VECTOR_ROBOT_IP`.
- Server port can be set with `PORT`.
- Browser accepts the server-sent default IP when no local IP has been saved.
- Telemetry watchdog no longer creates a new interval on every reconnect.
- `package.json` has `dev`, `start`, and `check` scripts.
- The main control surface is split into tabs for motors, camera, display, and
  audio so the first viewport is usable without a wall of stacked controls.
- Camera controls now use a single snapshot-polling implementation and no
  longer leave behind the broken duplicate MJPEG `<img>` path.
- Encoder rows accept both the current `motors` telemetry field and the older
  `motor` field, and `bun run check` now also bundles browser `public/app.js`
  so frontend syntax regressions are caught.

## Verification

Last local smoke test: 2026-05-24.

Commands:

```sh
cd tools/vector-web-ui
bun run check
PORT=3108 VECTOR_ROBOT_IP=192.168.1.89 bun run server.js
curl -I http://localhost:3108/
curl -I http://localhost:3108/app.js
curl -I http://localhost:3108/index.css
curl -H 'x-robot-ip: 127.0.0.1' http://localhost:3108/api/status
bun build public/app.js --target=browser --outfile=/tmp/vector-web-ui-app-check.js
```

Result:

- Static HTML, JS, and CSS returned `200 OK`.
- Browser page title was `Vector Robot Console`.
- In-app browser reload reported no console errors or warnings.
- `/api/status` returned expected proxy failure when pointed at
  `127.0.0.1` without a running robot API.
- The UI rendered new FACE DISPLAY and AUDIO panels.
- Camera tab interaction was verified locally: clicking `START STREAM` no
  longer throws a browser runtime error and reports `NO FRAME` when the robot
  snapshot producer has not supplied an image.
- Encoder updates were verified against a local mock robot API on
  `127.0.0.1:8080`; after WebSocket connect, rows rendered changing positions
  and deltas from `/v1/motors/state`.
- Responsive smoke test at `390x844` rendered the tabbed control surface with
  the drive panel hidden and no browser console errors.
- On robot `192.168.1.89`, the running `vector-hw-api` was hot-patched from
  `api_version=0.1.0` to `0.2.0` after UI controls revealed missing endpoints.
  `GO` now reaches `/v1/motors/position`; a lift `+100` tick command returned
  OK and moved the encoder. `HOLD` now calls robot-side `/v1/motors/hold`
  instead of browser polling relative correction moves.
- Camera controls now display robot-produced BMP snapshots from
  `/v1/camera/snapshot`. The default frame is now a 640x360 24-bit color BMP
  generated from Anki RGB888 shared-memory frames.
- The Bun proxy accepts `VECTOR_ROBOT_PORT` in addition to `VECTOR_ROBOT_IP`,
  which allows local validation against temporary API runs such as port `8081`
  without changing the browser code.

Robot validation on `192.168.1.89` after the firmware hot-patch:

- `/v1/display/init` returned OK with `panel=santek`.
- `/v1/display/frame` accepted a generated RGB565 test pattern.
- `/v1/audio/play` accepted a generated WAV and `aplay` reported playback.
- `/v1/camera/snapshot` returned `image/bmp` with a 640x360 visible color room
  frame during temporary 2026-05-25 validation on stock-like OS port 8081.
- `/v1/camera/stream` stayed connected and returned multipart BMP frames. After
  the RGB888 protocol rewrite and format-change slot-lock barrier, a 12s
  client-timeout run returned 58 fresh multipart parts during temporary
  2026-05-25 validation.
- Chrome displayed the proxied camera stream in the web UI as a canvas-rendered
  live feed. Playwright validation drew 38 `640x360` canvas frames in 8s,
  reported `LIVE`, and had no console warnings/errors.
- `/v1/motors/hold` enabled and disabled lift hold successfully via API.

## Known Issues

The UI is not production-grade yet.

- It is Bun-only because `server.js` uses `Bun.serve`, `Bun.file`, and
  `import.meta.dir`.
- The visual design is dense and dark; it has not been responsive-tested here.
- Sensor calibration thresholds are based on one observed unit and may be
  wrong for another robot or firmware revision.
- The UI assumes `/v1/events` can be consumed as SSE through the proxy.
- It does not yet expose app install/start/stop/delete endpoints.
- Camera stream display currently receives multipart BMP frames, not JPEG. The
  browser UI parses the multipart stream and draws BMP frames onto a canvas,
  using native `createImageBitmap(image/bmp)` when available and a manual 24-bit
  BMP decoder as fallback.
  This differs from wire-pod's stock-runtime path, which decodes robot
  `ImageChunk` data server-side and re-encodes multipart JPEG for the browser.
  The default firmware output is now RGB888-derived color.

The display controls resize still images or video frames to the robot face
canvas (`184x96`), convert pixels to little-endian RGB565, and POST the exact
35,328-byte binary frame to `/api/display/frame` through the Bun proxy. Video
streaming currently sends a frame every 120 ms; it is intended for validation,
not final high-FPS animation. For selected video files, `PLAY VIDEO` also
attempts to decode the video's audio track in the browser and send it to the
robot with the same chunked WAV path used by standalone audio uploads. Large
video files are referenced through browser object URLs rather than copied into
the Bun server; the object URL is revoked after playback ends or a new media
file is selected.

The audio controls decode browser-supported audio files, resample/downmix them
to 16 kHz mono 16-bit PCM WAV in the browser, split long audio into WAV chunks
below the robot API's 4 MiB upload limit, and upload those chunks sequentially
to `/api/audio/play`. Each chunk is proxied to `vector-hw-api` and starts
`aplay` on the robot. The volume slider calls `/api/audio/volume`, which maps
to the robot's ALSA `RX3 Digital Volume` mixer control, so volume changes do
not require re-encoding the current audio chunk. The stop button cancels any
pending browser-side chunks and calls `/api/audio/stop`.

The Bun proxy buffers non-GET request bodies before forwarding them to the
robot and sets `Content-Length` explicitly. This is required because the robot
API's simple HTTP parser reads binary uploads by `Content-Length` and does not
handle chunked request bodies.

The current 50 ms motor command loop is a workaround for the API exposing only
raw motor power. Once `vector-hw-api` owns closed-loop velocity/position
control, the UI should send target intents rather than continuous raw motor
commands.

## Safety Notes

The server sends motor commands only while at least one browser WebSocket
client is connected. If the browser disconnects, targets are reset to zero.
The robot-side `vector-hw-api` TTL watchdog remains the main safety mechanism.
