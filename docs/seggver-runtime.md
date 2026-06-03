# WireOS Runtime On Seggver

`seggver` is the canonical always-on runtime host for WireOS tooling that does
not need to run on the robot itself. The Mac remains the source checkout and
Yocto build machine; `seggver` runs the stable LAN services.

## Goals

- Keep browser access stable at one LAN URL.
- Keep MCP execution close to the robot and independent of laptop port
  conflicts.
- Put optional TTS synthesis on the server instead of the robot or Mac.
- Treat Vector as a physical device that can be off, asleep, discharged, or on
  a different DHCP lease without making deploys fail.

## Canonical Paths

Local source:

```text
/Users/velizard/Projects/wire-os
```

Server runtime:

```text
egg@seggver:/home/egg/wire-os-runtime
```

Server layout:

```text
/home/egg/wire-os-runtime/source/tools/vector-web-ui
/home/egg/wire-os-runtime/source/tools/vector-robot-sdk
/home/egg/wire-os-runtime/runtime/docker-compose.yml
/home/egg/wire-os-runtime/bin/wireos-vector-mcp
```

LAN endpoints:

```text
Web UI: http://192.168.1.63:9786/
Robot API default: http://vector.home:8080/v1/status
```

## One Command

From the local checkout:

```sh
scripts/seggver-runtime.sh
```

The default command deploys the current local web UI and SDK to `seggver`,
installs/updates the Docker Compose runtime, enables the user systemd unit,
starts the service, creates the local MCP SSH wrapper, points the local LM
Studio MCP configs at that wrapper, and prints health.

Useful subcommands:

```sh
scripts/seggver-runtime.sh status
scripts/seggver-runtime.sh github-status
scripts/seggver-runtime.sh deploy-github
scripts/seggver-runtime.sh restart
scripts/seggver-runtime.sh logs
scripts/seggver-runtime.sh mcp-test
```

Optional overrides:

```sh
VECTOR_ROBOT_IP=192.168.1.93 scripts/seggver-runtime.sh
WIREOS_WEB_PORT=9787 scripts/seggver-runtime.sh
```

Prefer defaults. Overrides are for temporary validation only.

## Runtime Design

The server runs the web UI through Docker Compose with `oven/bun:1`. There are
two source modes:

- `deploy`: fast development sync from the local Mac checkout into
  `/home/egg/wire-os-runtime/source`.
- `deploy-github`: clean server checkout from
  `https://github.com/VelizarSeleznev/wire-os.git` branch `main` into
  `/home/egg/wire-os-runtime/repo`.

Use `deploy` while iterating before commit. Use `deploy-github` for the stable
runtime after changes are committed and pushed. `deploy-github` cannot see
local uncommitted files.

The MCP path is intentionally thin:

```text
local MCP client -> /Users/velizard/bin/wireos-vector-mcp -> ssh egg@seggver -> vector_mcp.py -> robot
```

This still uses stdio MCP for clients that expect it, but the real Python SDK
and robot network calls happen on `seggver`. The Mac side is only an SSH pipe.
The deploy helper updates both known LM Studio MCP config files:

```text
/Users/velizard/.cache/lm-studio/mcp.json
/Users/velizard/.lmstudio/mcp.json
```

## Robot Availability Contract

The robot is not an always-on server. Agents should report these states
separately:

- `runtime down`: Docker/systemd/web UI on `seggver` is not healthy.
- `robot offline`: `vector.home:8080` or the configured robot IP does not
  answer.
- `robot API degraded`: `/v1/status` answers but reports missing hardware or
  stale telemetry.

`robot offline` is not a failed deployment. It usually means the robot is off,
asleep, discharged, rebooting, or has moved DHCP leases. First check charger
state and the router/DHCP view before changing software.

## Development Workflow

1. Edit code locally in `/Users/velizard/Projects/wire-os`.
2. Run local syntax checks where available:

   ```sh
   cd tools/vector-web-ui
   bun run check
   ```

3. Deploy to server:

   ```sh
   scripts/seggver-runtime.sh
   ```

4. Before considering work finished, check whether server runtime changes are
   published:

   ```sh
   scripts/seggver-runtime.sh github-status
   ```

5. After committing and pushing relevant changes, switch the server to the
   GitHub-backed runtime:

   ```sh
   scripts/seggver-runtime.sh deploy-github
   ```

6. Open the stable UI:

   ```text
   http://192.168.1.63:9786/
   ```

7. For MCP changes, validate:

   ```sh
   scripts/seggver-runtime.sh mcp-test
   ```

8. For robot behavior changes, validate against the physical robot only when it
   is awake and reachable, then update `docs/vector-hardware-status.md`.

## TTS Placement

TTS should start as a server-side service on `seggver`, not inside the robot
firmware. The first implementation should synthesize a WAV on the server and
play it through:

```text
POST http://<robot>:8080/v1/audio/play
```

For GLaDOS specifically, prefer the Piper-compatible path:

- server stores `glados.onnx` and `glados.onnx.json`;
- server runs Piper/espeak in a container;
- generated WAV is downmixed/resampled if needed and sent to the robot audio
  endpoint;
- robot only handles WAV playback.

Only move TTS onto the robot after measuring RSS, synthesis latency, and audio
stability on the physical device.

## Operational Commands

Server-side checks:

```sh
ssh egg@seggver 'systemctl --user status wireos-vector-runtime.service'
ssh egg@seggver 'cd /home/egg/wire-os-runtime/runtime && docker compose ps'
ssh egg@seggver 'cd /home/egg/wire-os-runtime/runtime && docker compose logs --tail=100'
```

Robot-side checks from server:

```sh
ssh egg@seggver 'curl -fsS --max-time 4 http://vector.home:8080/v1/status'
ssh egg@seggver 'curl -fsS --max-time 4 http://192.168.1.93:8080/v1/status'
```

Do not start ad hoc web UI instances on random Mac ports unless debugging a
local-only frontend issue. After any web UI or SDK change intended for normal
use, run `scripts/seggver-runtime.sh`; after publishing it, run
`scripts/seggver-runtime.sh deploy-github`.
