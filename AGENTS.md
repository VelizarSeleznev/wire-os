# AGENTS.md - WireOS Vector Hardware Workspace

Scope: this file applies to everything under `/Users/velizard/Projects/wire-os`.

## Prime Directive

This repository is the canonical workspace for the Vector minimal hardware
firmware work and its companion web UI. Treat this directory as the project
root. Do not use chat memory or files from `/Users/velizard` as project state
unless a task explicitly requires global machine inventory.

Before changing code, read:

- `docs/project-workspace.md`
- `docs/vector-hw-image.md`
- `docs/vector-hardware-status.md`
- `docs/vector-web-ui.md` when touching the browser control UI

When hardware support, API behavior, robot validation, build artifacts, or UI
behavior changes, update the relevant docs in the same change. In particular,
keep `docs/vector-hardware-status.md` current so project status can be
understood without re-reading implementation code.

## Workspace Layout

- Firmware/source checkout: `/Users/velizard/Projects/wire-os`
- Full Yocto build checkout on case-sensitive APFS:
  `/Volumes/wire-os-cs/wire-os`
- Built hardware image artifacts:
  `/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts`
- Companion control UI source:
  `tools/vector-web-ui`

The case-sensitive checkout exists because the Linux 3.18 tree contains paths
that differ only by case. Do not run full kernel/image builds from a
case-insensitive APFS checkout.

## Build Rules

Use Docker image `vic-yocto-builder-7` and named volume `wireos-yocto-tmp-cs`
for full Yocto builds on this Mac. The successful command is recorded in
`docs/vector-hw-image.md`.

Do not delete or overwrite `/Volumes/wire-os-cs/wire-os/build/hwdev-artifacts`
unless the user asks for a rebuild/export.

## Change Boundaries

Keep Anki personality/runtime out of the hardware image:

- no `victor`
- no `vic-*`
- no `wired`
- no `anki-robot-target`

It is acceptable to reuse low-level BSP, Qualcomm, camera/audio, boot, OTA,
Wi-Fi, SSH, `/data`, and systemd infrastructure.

`vector-hw-api` is the single owner of robot hardware devices. Extension apps
and web UI code must use HTTP/WebSocket API calls instead of opening device
nodes directly.

## Dirty Worktree Notes

There may be kernel-generated files in the main checkout from an earlier
failed case-insensitive kernel build. Do not revert them unless the user asks.
Ignore unrelated dirty files when working on `tools/vector-web-ui`, docs, or
the minimal image recipes.

## Web UI

The canonical copy of the robot control site is `tools/vector-web-ui`.
The older sibling folder `/Users/velizard/Projects/vector-web-ui` may still
exist, but new work should happen inside this repository.

Run it with:

```sh
cd tools/vector-web-ui
VECTOR_ROBOT_IP=<robot-ip> bun run dev
```

Default URL: `http://localhost:3000`.
