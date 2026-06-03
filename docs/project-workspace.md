# Project Workspace

This directory is the canonical workspace for the minimal Vector hardware OS
work. Open new agent sessions at:

```text
/Users/velizard/Projects/wire-os
```

The project contains two related parts:

- Minimal WireOS/Linux image and hardware API.
- Companion LAN web UI for controlling the robot through that API.

For current hardware capability status, read:

```text
docs/vector-hardware-status.md
```

Keep that file updated whenever robot hardware support or validation status
changes, so future sessions can understand project state without reading code.

## Important Paths

```text
/Users/velizard/Projects/wire-os
```

Primary source/documentation workspace.

```text
/Volumes/wire-os-cs/wire-os
```

Case-sensitive checkout used for full Yocto builds on macOS. The Linux kernel
tree needs case-sensitive paths.

```text
/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts
```

Last successful exported hardware-image artifacts:

- `vicos-20180309123456.ota`
- `vicos-20260522212342.ota`
- `vicos-20260531091618.ota`
- `apq8009-robot-boot.img`
- `zImage-dtb-apq8009-robot.bin`
- `machine-hw-image-apq8009-robot.rootfs-20260531091511.ext4`
- `machine-hw-image-apq8009-robot.rootfs-20260531091511.manifest`
- `machine-hw-image-apq8009-robot.rootfs-20260531091511.testdata.json`
- `machine-hw-image-apq8009-robot.rootfs-20260522113639.ext4`
- `machine-hw-image-apq8009-robot.rootfs-20260522113639.manifest`
- `machine-hw-image-apq8009-robot.rootfs-20260522113639.testdata.json`
- `ota-manifest.ini`

```text
tools/vector-web-ui
```

Canonical web UI source. The old sibling directory
`/Users/velizard/Projects/vector-web-ui` is not the canonical copy anymore.

```text
scripts/seggver-runtime.sh
```

One-command deploy/status helper for the always-on server runtime on
`seggver`. It syncs `tools/vector-web-ui` and `tools/vector-robot-sdk` to
`egg@seggver:/home/egg/wire-os-runtime`, starts the Docker Compose web UI, and
installs the MCP SSH wrapper. It also supports `deploy-github`, which runs from
a clean server checkout of `VelizarSeleznev/wire-os:main`. See
`docs/seggver-runtime.md`.

```text
tools/vector-robot-sdk
```

Local LLM/client tooling for the robot HTTP API:

- `vector_robot.py`: dependency-free Python client.
- `vectorctl.py`: CLI for status, sensors, motors, hold, LEDs, and snapshots.
- `vector_mcp.py`: minimal stdio MCP-compatible tool server for LLM clients.

See `docs/vector-mcp.md` for the MCP tool contract, including the motor map
exposed to models: `0=left_track`, `1=right_track`, `2=lift`, `3=head`.

Behavior-level planning lives in:

```text
docs/vector-high-level-behaviors.md
```

Use it for calibrated lift/head movement, safe hold, DDL animation playback,
charger docking, beamforming, and other features built above the raw hardware
API.

## Current Verified State

On 2026-05-22, `machine-hw-image` built successfully:

```text
Tasks Summary: Attempted 3897 tasks of which 3865 didn't need to be rerun and all succeeded.
```

The generated manifest includes:

- `vector-hw-api`
- `vector-hw-cli`
- `vector-app-runner`
- `python3-core`
- `python3-json`
- `python3-netclient`
- `python3-modules`
- `openssh`
- `audiohal`
- `mm-camera`
- `rmtstorage`
- `update-engine`
- `update-os`

The generated manifest does not include:

- `victor`
- `vic-*`
- `wired`
- `anki-robot-target`

On 2026-05-31, the robot was reachable as `vector.home` at `192.168.1.93`
rather than the older `192.168.1.89` address. OTA
`vicos-20260531091618.ota` was deployed and the API returned after reboot.
Robot-side Python script upload required a post-OTA hotfix install of the
prepared ARMv7 Python runtime tarball because `/usr/bin/python3` was absent on
the booted system.

For non-robot tooling, the preferred stable runtime is now `seggver`:

```text
http://192.168.1.63:9786/
```

Deploy or inspect it with:

```sh
scripts/seggver-runtime.sh
scripts/seggver-runtime.sh github-status
scripts/seggver-runtime.sh deploy-github
scripts/seggver-runtime.sh status
```

## Known Gaps

The image has not yet been flashed and validated on physical Vector hardware.

Hardware API implementation is first-pass:

- Motors have TTL safety and zero-on-exit behavior.
- Spine/display/IMU/camera/audio access is still partly adapter-level.
- LCD frame upload lacks production display init/DC/reset sequencing.
- Camera endpoint currently serves a snapshot file path, not a live capture
  producer.
- Audio endpoint reports status only until a stable HAL path is chosen.

See `docs/vector-hardware-status.md` for the detailed feature-by-feature
status table and roadmap.

## New Session Checklist

1. Open `/Users/velizard/Projects/wire-os`.
2. Read `AGENTS.md`.
3. Read `docs/vector-hw-image.md`.
4. Read `docs/vector-hardware-status.md`.
5. Read `docs/seggver-runtime.md` if working on web UI, MCP, TTS, or server
   runtime.
6. Read `docs/vector-web-ui.md` if working on the browser control site.
7. Use `/Volumes/wire-os-cs/wire-os` only for full Yocto builds.
