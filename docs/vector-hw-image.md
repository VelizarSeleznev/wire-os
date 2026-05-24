# Vector Hardware API Image

`machine-hw-image` is a minimal WireOS/Linux image for Vector that keeps the
APQ8009 BSP, boot/OTA layout, Wi-Fi, SSH, `/data`, systemd, and selected
Qualcomm media packages, but does not install the Anki personality runtime.

The image installs one primary hardware owner:

- `vector-hw-api.service`: HTTP/WebSocket daemon on `0.0.0.0:8080`.
- `vector-hw-cli`: local recovery/test client.
- `vector-appctl`: app bundle manager for `/data/vector-apps`.
- `vector-app@.service`: systemd template for local extension apps.
- `vector-app-runner.service`: boot-time scanner for autostart apps in `/data`.

Current hardware capability status is tracked in:

```text
docs/vector-hardware-status.md
```

Update that file whenever API behavior, robot validation, or hardware support
changes.

## Build

From the repo root on a supported Linux build host:

```sh
./build/build.sh -bt hwdev -bp <boot-password> -v <build-increment>
```

The `hwdev` build type uses Yocto target `machine-hw-image` and keeps the
normal WireOS dev OTA/signing path. It intentionally skips the `anki/victor`,
`anki/wired`, and `external/purplpkg` submodule checks because the hardware
image does not build the Anki personality or web stack.

For direct Yocto work:

```sh
cd poky
source build/conf/set_bb_env.sh
build-hwdev
```

### Verified Local Build

On 2026-05-22 this image was built successfully in the case-sensitive macOS
workspace:

```text
/Volumes/wire-os-cs/wire-os
```

The Yocto tmp directory was kept in Docker named volume:

```text
wireos-yocto-tmp-cs
```

The successful command was:

```sh
docker run --rm \
  -v /Volumes/wire-os-cs/wire-os:/Volumes/wire-os-cs/wire-os \
  -v wireos-yocto-tmp-cs:/Volumes/wire-os-cs/wire-os/poky/build/tmp-glibc \
  -w /Volumes/wire-os-cs/wire-os/poky \
  -v /Volumes/wire-os-cs/wire-os/build/cache:/home/velizard/.ccache \
  -v /Volumes/wire-os-cs/wire-os/build/usercache:/home/velizard/.cache \
  vic-yocto-builder-7 \
  bash -lc 'sudo chown -R $(id -u):$(id -g) build/tmp-glibc && source build/conf/set_bb_env.sh >/tmp/setenv.log && unset_bb_env && export MACHINE=apq8009-robot DISTRO=msm-perf VARIANT=perf PRODUCT=robot DEV=1 && cdbitbake machine-hw-image'
```

Result:

```text
Tasks Summary: Attempted 3897 tasks of which 3865 didn't need to be rerun and all succeeded.
```

Build artifacts were exported from the Docker volume to:

```text
/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts
```

Current exported files:

```text
apq8009-robot-boot.img
zImage-dtb-apq8009-robot.bin
machine-hw-image-apq8009-robot.rootfs-20260522113639.ext4
machine-hw-image-apq8009-robot.rootfs-20260522113639.manifest
machine-hw-image-apq8009-robot.rootfs-20260522113639.testdata.json
ota-manifest.ini
vicos-20180309123456.ota
vicos-20260522212342.ota
```

The OTA was generated with the existing `ota/Makefile` using the built
`apq8009-robot-boot.img` and the hardware system image. `machine-hw-image`
does not currently generate `/etc/os-version`; the local OTA generation used
`/etc/version` as `OS_VERSION_FILE`.

The manifest confirms the hardware image includes `vector-hw-api`,
`vector-hw-cli`, `openssh`, `audiohal`, `mm-camera`, and `rmtstorage`, while
`victor`, `vic-*`, `wired`, and `anki-robot-target` are absent.

### Docker Desktop Notes

On macOS Docker Desktop, keep Yocto `tmp-glibc` on a Docker named volume rather
than a host bind mount. Some native sysroot tasks use hardlinks heavily and can
fail with `Input/output error` on the macOS shared filesystem.

Example:

```sh
docker run --rm \
  -v "$PWD":"$PWD" \
  -v wireos-yocto-tmp:"$PWD/poky/build/tmp-glibc" \
  -w "$PWD/poky" \
  vic-yocto-builder-7 \
  bash -lc 'source build/conf/set_bb_env.sh && build-hwdev'
```

The full kernel/image build also needs a case-sensitive source filesystem.
Linux 3.18 contains distinct files such as `xt_TCPMSS.c` and `xt_tcpmss.c`;
case-insensitive APFS can collapse those paths and break `linux-msm:do_compile`
with:

```text
No rule to make target 'net/netfilter/xt_TCPMSS.o'
```

Use a Linux host, a case-sensitive APFS volume/disk image, or keep the complete
checkout inside a Docker volume before attempting full `machine-hw-image`/OTA
builds on macOS.

For this local Mac, Docker Desktop disk was increased to 128 GiB before the
successful image build. The case-sensitive checkout also needed source/prebuilt
paths materialized for Qualcomm camera/audio/storage recipes, including
`prebuilt_HY11/apq8009-robot/mm-camera`, `prebuilt_HY11/apq8009-robot/adsprpc`,
`audio/mm-audio/sound_trigger`, `hardware/qcom/keymaster`, and `remotefs`.

The first build may also need the Yocto download cache populated from the host
if Docker's internal network fetches time out. The cache path is:

```text
poky/build/downloads
```

## Image Composition

The image recipe is:

```text
poky/victor/meta-anki/recipes-products/images/machine-hw-image.bb
```

The APQ8009 package list is:

```text
poky/victor/meta-anki/recipes-products/images/apq8009/apq8009-robot-hw-image.inc
```

It starts from the Qualcomm `apq8009-robot-image.inc` base and adds platform
services needed for a remotely managed hardware endpoint:

- `/data` support: `cryptsetup`, `user-data-locker`, storage dependencies.
- Network/developer access: `connman`, `wpa-supplicant`, `openssh` from base.
- Update path: `update-engine`, `update-os`, and existing OTA machinery.
- Media support packages: `audiohal`, `alsa-utils`, `adsprpc`, `init-audio`,
  `mm-camera`, `rmtstorage`.
- Diagnostics: `htop`, `procps`, `net-tools`, `util-linux-dmesg` on non-user
  builds.

It explicitly removes the Anki personality/service graph:

```text
anki-robot-target victor vic-* wired
```

## API Surface

`vector-hw-api` listens on `0.0.0.0:8080` with no auth in the first version.

Implemented endpoints:

- `GET /v1/status`
- `GET /v1/sensors`
- `GET /v1/motors/state`
- `POST /v1/motors`
- `POST /v1/motors/position`
- `POST /v1/motors/hold`
- `POST /v1/motors/stop`
- `POST /v1/leds/backpack`
- `POST /v1/display/brightness`
- `POST /v1/display/init`
- `POST /v1/display/frame`
- `GET /v1/camera/snapshot`
- `GET /v1/camera/stream`
- `POST /v1/camera/daemon/start`
- `POST /v1/camera/daemon/stop`
- `GET /v1/audio/status`
- `POST /v1/audio/play`
- `POST /v1/audio/stop`
- `POST /v1/audio/volume`
- `GET /v1/events`
- `GET /v1/apps`
- `POST /v1/apps/install`
- `POST /v1/apps/<id>/start`
- `POST /v1/apps/<id>/stop`
- `DELETE /v1/apps/<id>`

`/v1/events` supports WebSocket upgrade. A plain HTTP `GET` returns an SSE
stream for simple clients.

`/v1/display/init` initializes the face LCD GPIO reset/DC lines, configures
`/dev/spidev1.0`, detects the panel generation from EMR, runs the original
Santek/Midas register scripts from `anki/rampost/lcd.c`, clears the panel, and
turns it on. `/v1/display/frame` accepts exactly `184x96x2` bytes of little
endian RGB565. Santek panels receive the full frame; Midas panels are scaled to
`160x80`.

`/v1/audio/play` accepts an uploaded WAV/PCM file up to 4 MiB, stores it at
`/tmp/vector-hw-audio.wav`, runs the existing audio mixer init script, and
applies the current `/v1/audio/volume` mixer setting before launching `aplay`.
`/v1/audio/stop` kills any running `aplay` instance. `/v1/audio/volume`
accepts JSON `{ "level": 0..100 }` and maps it to the robot's `RX3 Digital
Volume` ALSA control (`amixer cset numid=33`).

## Deploy Helper Skill

A local Codex skill was added for repeatable build/package/deploy operations:

```text
/Users/velizard/.codex/skills/wire-os-vector-deploy
```

Useful commands:

```sh
/Users/velizard/.codex/skills/wire-os-vector-deploy/scripts/vector_hw_build_deploy.sh build
/Users/velizard/.codex/skills/wire-os-vector-deploy/scripts/vector_hw_build_deploy.sh package
/Users/velizard/.codex/skills/wire-os-vector-deploy/scripts/vector_hw_build_deploy.sh deploy 192.168.1.89
```

On 2026-05-22, `vicos-20260522212342.ota` was packaged successfully after
adding `update-os` to the hardware image. The currently installed robot image
at `192.168.1.89` did not contain `update-os`, `/anki/bin/update-engine`, or
`update-engine.service`, so the OTA could not be triggered from that running
image. The updated `vector-hw-api` and `vector-hw-cli` were copied over SSH as
a hot-patch after remounting `/` read-write, and `vector-hw-api.service` was
restarted successfully.

## Hardware Ownership

Only `vector-hw-api` should open direct hardware devices:

- Spine/body MCU: `/dev/ttyHS0`
- Face LCD SPI: `/dev/spidev1.0`
- IMU SPI presence check: `/dev/spidev0.0`
- Face backlight: `/sys/class/leds/face-backlight*`

Extension apps must call the local API instead of opening devices directly.

Motor commands require a TTL. The daemon defaults to `250 ms`, clamps to
`5000 ms`, and zeros motor power after expiry. On service stop/crash, systemd
runs:

```sh
/usr/bin/vector-hw-api --stop-motors
```

That one-shot path writes zero motor frames directly to `/dev/ttyHS0`; it does
not depend on the HTTP daemon still being alive.

## Local Apps

App bundles are installed under:

```text
/data/vector-apps/<app-id>
```

Bundle format:

```text
manifest.json
bin/...
data/...
```

Manifest v1:

```json
{
  "id": "demo-drive",
  "name": "Demo Drive",
  "version": "0.1.0",
  "command": "./bin/demo-drive",
  "env": {
    "VECTOR_HW_API": "http://127.0.0.1:8080"
  },
  "restart": "on-failure",
  "autostart": false
}
```

`vector-appctl install <bundle.tar.gz>` validates the manifest, extracts the
bundle, writes a `/data/vector-apps/<id>/run` wrapper, and manages
`vector-app@<id>.service`. Apps run as the dedicated `vectorapp` user.

The system rootfs is read-only, so per-app systemd overrides are written to
`/run/systemd/system` at install/start time. On reboot,
`vector-app-runner.service` scans `/data/vector-apps`, rebuilds those runtime
overrides, and starts only apps whose manifest has `"autostart": true`.

## Current Limitations

This is the first integration pass. Spine telemetry, motor TTL safety,
backpack LEDs, backlight control, app install/start/stop/delete, and API
plumbing are implemented. The display frame endpoint currently writes raw
payload bytes to the LCD SPI device and still needs the full LCD init/DC/reset
sequence before it should be treated as production display support.

Camera snapshot now captures from the original `mm-anki-camera` Unix datagram
socket/shared-memory path and serves `/tmp/vector-camera-snapshot.bmp` as a
640x360 grayscale BMP generated from RAW10 frames. The current output is usable
for validation snapshots but still needs a proper color/demosaic pipeline.

Additional hardware notes:

- Motor encoder telemetry is present in Spine frames and exposed as
  `motor[].position`, `motor[].delta`, and `motor[].time`. `POST
  /v1/motors/position` handles relative encoder moves, and `POST
  /v1/motors/hold` now runs a robot-side closed-loop hold for one motor.
- Motion sensors/IMU are still suspect on the observed robot: the SPI path
  returns a stable WHO_AM_I but accel/gyro values remain effectively constant.
- The image contains `mm-camera`, `mm-qcamera-daemon`, and `mm-anki-camera`
  binaries. The API now starts `mm-qcamera-daemon` and
  `mm-anki-camera-wrapper -C`, then reads the Anki shared-memory frame buffer.
  The stock `mm-anki-camera.service` still fails because the minimal image does
  not create the `camera` group expected by that service.
- Proximity/TOF and the second touch sensor are not proven working on the
  observed robot; treat their raw telemetry as diagnostic until validated.
