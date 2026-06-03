#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SERVER="${WIREOS_SERVER:-egg@seggver}"
SERVER_ROOT="${WIREOS_SERVER_ROOT:-/home/egg/wire-os-runtime}"
SERVER_SOURCE="$SERVER_ROOT/source"
SERVER_GIT_SOURCE="$SERVER_ROOT/repo"
SERVER_RUNTIME="$SERVER_ROOT/runtime"
WEB_PORT="${WIREOS_WEB_PORT:-9786}"
ROBOT_HOST="${VECTOR_ROBOT_IP:-vector.home}"
ROBOT_PORT="${VECTOR_ROBOT_PORT:-8080}"
ANIMATIONS_ROOT="${VECTOR_ANIMATIONS_ROOT:-/srv/eggnest/vector/vector-animations-build/assets}"
LOCAL_MCP_WRAPPER="${WIREOS_LOCAL_MCP_WRAPPER:-/Users/velizard/bin/wireos-vector-mcp}"
REPO_URL="${WIREOS_REPO_URL:-https://github.com/VelizarSeleznev/wire-os.git}"
REPO_BRANCH="${WIREOS_REPO_BRANCH:-main}"
COMPOSE_SOURCE_DIR="${WIREOS_COMPOSE_SOURCE_DIR:-../source}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [deploy|deploy-github|github-status|status|restart|logs|mcp-test]

Default command is deploy.

Environment overrides:
  WIREOS_SERVER=$SERVER
  WIREOS_SERVER_ROOT=$SERVER_ROOT
  WIREOS_WEB_PORT=$WEB_PORT
  VECTOR_ROBOT_IP=$ROBOT_HOST
  VECTOR_ROBOT_PORT=$ROBOT_PORT
  VECTOR_ANIMATIONS_ROOT=$ANIMATIONS_ROOT
  WIREOS_REPO_URL=$REPO_URL
  WIREOS_REPO_BRANCH=$REPO_BRANCH
EOF
}

remote() {
  ssh -o BatchMode=yes "$SERVER" "$@"
}

write_remote_file() {
  local path="$1"
  remote "mkdir -p '$(dirname "$path")' && cat > '$path'"
}

deploy_files() {
  remote "mkdir -p '$SERVER_SOURCE/tools/vector-web-ui' '$SERVER_SOURCE/tools/vector-robot-sdk' '$SERVER_RUNTIME' '$SERVER_ROOT/bin'"

  rsync -az --delete \
    --exclude '.git/' \
    --exclude 'node_modules/' \
    --exclude '.venv/' \
    --exclude '__pycache__/' \
    --exclude 'build/' \
    "$ROOT/tools/vector-web-ui/" "$SERVER:$SERVER_SOURCE/tools/vector-web-ui/"

  rsync -az --delete \
    --exclude '.venv/' \
    --exclude '__pycache__/' \
    "$ROOT/tools/vector-robot-sdk/" "$SERVER:$SERVER_SOURCE/tools/vector-robot-sdk/"

  COMPOSE_SOURCE_DIR="../source"
}

deploy_github_files() {
  remote "set -e
    mkdir -p '$SERVER_ROOT'
    if [ ! -d '$SERVER_GIT_SOURCE/.git' ]; then
      rm -rf '$SERVER_GIT_SOURCE'
      git clone --branch '$REPO_BRANCH' '$REPO_URL' '$SERVER_GIT_SOURCE'
    else
      cd '$SERVER_GIT_SOURCE'
      git remote set-url origin '$REPO_URL'
      git fetch --prune origin '$REPO_BRANCH'
      git reset --hard 'origin/$REPO_BRANCH'
      git clean -fdx
    fi"

  COMPOSE_SOURCE_DIR="../repo"
}

install_runtime_files() {
  cat <<EOF | write_remote_file "$SERVER_RUNTIME/.env"
WIREOS_WEB_PORT=$WEB_PORT
VECTOR_ROBOT_IP=$ROBOT_HOST
VECTOR_ROBOT_PORT=$ROBOT_PORT
VECTOR_ANIMATIONS_ROOT=$ANIMATIONS_ROOT
WIREOS_SOURCE_DIR=$COMPOSE_SOURCE_DIR
EOF

  cat <<'EOF' | write_remote_file "$SERVER_RUNTIME/docker-compose.yml"
services:
  vector-web-ui:
    image: oven/bun:1
    container_name: wireos-vector-web-ui
    restart: unless-stopped
    working_dir: /work/tools/vector-web-ui
    command: ["bun", "run", "server.js"]
    environment:
      PORT: "${WIREOS_WEB_PORT:-9786}"
      VECTOR_ROBOT_IP: "${VECTOR_ROBOT_IP:-vector.home}"
      VECTOR_ROBOT_PORT: "${VECTOR_ROBOT_PORT:-8080}"
      VECTOR_ANIMATIONS_ROOT: "${VECTOR_ANIMATIONS_ROOT:-/srv/eggnest/vector/vector-animations-build/assets}"
    ports:
      - "${WIREOS_WEB_PORT:-9786}:${WIREOS_WEB_PORT:-9786}/tcp"
      - "5005:5005/udp"
    volumes:
      - "${WIREOS_SOURCE_DIR:-../source}:/work:ro"
      - /srv/eggnest/vector:/srv/eggnest/vector:ro
EOF

  cat <<EOF | write_remote_file "$SERVER_ROOT/bin/wireos-vector-mcp"
#!/usr/bin/env bash
set -euo pipefail
cd "$SERVER_SOURCE/tools/vector-robot-sdk"
exec /usr/bin/python3 vector_mcp.py --host "\${VECTOR_ROBOT_IP:-$ROBOT_HOST}" --port "\${VECTOR_ROBOT_PORT:-$ROBOT_PORT}"
EOF

  remote "chmod +x '$SERVER_ROOT/bin/wireos-vector-mcp'"

  cat <<EOF | write_remote_file "/home/egg/.config/systemd/user/wireos-vector-runtime.service"
[Unit]
Description=WireOS Vector server runtime
After=network-online.target

[Service]
Type=oneshot
WorkingDirectory=$SERVER_RUNTIME
RemainAfterExit=yes
ExecStart=/usr/bin/docker compose up -d
ExecStop=/usr/bin/docker compose down
TimeoutStartSec=180

[Install]
WantedBy=default.target
EOF

  remote "systemctl --user daemon-reload && systemctl --user enable wireos-vector-runtime.service >/dev/null"
}

install_local_wrappers() {
  mkdir -p "$(dirname "$LOCAL_MCP_WRAPPER")"
  cat > "$LOCAL_MCP_WRAPPER" <<EOF
#!/usr/bin/env bash
set -euo pipefail
exec ssh -o BatchMode=yes "$SERVER" "$SERVER_ROOT/bin/wireos-vector-mcp"
EOF
  chmod +x "$LOCAL_MCP_WRAPPER"

  LOCAL_MCP_WRAPPER="$LOCAL_MCP_WRAPPER" python3 - <<'PY'
import json
import os
from pathlib import Path

wrapper = os.environ["LOCAL_MCP_WRAPPER"]
paths = [
    Path.home() / ".cache/lm-studio/mcp.json",
    Path.home() / ".lmstudio/mcp.json",
]

for path in paths:
    if path.exists():
        data = json.loads(path.read_text())
    else:
        data = {}
    servers = data.setdefault("mcpServers", {})
    servers["wireos-vector"] = {
        "command": wrapper,
        "args": [],
        "cwd": str(Path.home()),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"updated {path}")
PY
}

start_runtime() {
  remote "cd '$SERVER_RUNTIME' && docker compose pull && systemctl --user restart wireos-vector-runtime.service"
}

status() {
  echo "server: $SERVER"
  remote "cd '$SERVER_RUNTIME' 2>/dev/null && docker compose ps || true"
  echo
  echo "web:"
  if curl -fsS --max-time 5 "http://192.168.1.63:$WEB_PORT/api/config" >/tmp/wireos-web-config.json 2>/dev/null; then
    cat /tmp/wireos-web-config.json
    echo
  else
    echo "offline: http://192.168.1.63:$WEB_PORT/api/config did not answer"
  fi
  echo
  echo "robot:"
  if remote "curl -fsS --max-time 4 'http://$ROBOT_HOST:$ROBOT_PORT/v1/status' >/tmp/wireos-robot-status.json"; then
    remote "python3 - <<'PY'
import json
p='/tmp/wireos-robot-status.json'
data=json.load(open(p))
print('online', 'api=' + str(data.get('api_version', 'unknown')), 'spine=' + str(data.get('spine', {}).get('connected', 'unknown')))
PY"
  else
    echo "offline or asleep: $ROBOT_HOST:$ROBOT_PORT did not answer"
  fi
  echo
  echo "urls:"
  echo "  web: http://192.168.1.63:$WEB_PORT/"
  echo "  mcp wrapper: $LOCAL_MCP_WRAPPER"
  echo
  echo "source:"
  remote "if [ -d '$SERVER_GIT_SOURCE/.git' ]; then cd '$SERVER_GIT_SOURCE' && echo \"  github repo: \$(git rev-parse --short HEAD) \$(git rev-parse --abbrev-ref HEAD)\"; fi; if [ -d '$SERVER_SOURCE' ]; then echo '  local sync: present'; fi; [ -f '$SERVER_RUNTIME/.env' ] && grep '^WIREOS_SOURCE_DIR=' '$SERVER_RUNTIME/.env' | sed 's/^/  /'"
}

mcp_test() {
  printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"wireos-runtime-smoke","version":"0"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
    | "$LOCAL_MCP_WRAPPER"
}

github_status() {
  echo "local:"
  git -C "$ROOT" status --short
  echo
  echo "local HEAD:"
  git -C "$ROOT" log --oneline --decorate --max-count=3
  echo
  echo "origin:"
  git -C "$ROOT" ls-remote --heads origin "$REPO_BRANCH"
  echo
  echo "server github checkout:"
  remote "if [ -d '$SERVER_GIT_SOURCE/.git' ]; then cd '$SERVER_GIT_SOURCE' && git log --oneline --decorate --max-count=3 && git status --short; else echo 'missing: run deploy-github after pushing changes'; fi"
}

cmd="${1:-deploy}"
case "$cmd" in
  deploy)
    deploy_files
    install_runtime_files
    install_local_wrappers
    start_runtime
    status
    ;;
  deploy-github)
    deploy_github_files
    install_runtime_files
    install_local_wrappers
    start_runtime
    status
    ;;
  github-status)
    github_status
    ;;
  status)
    status
    ;;
  restart)
    remote "systemctl --user restart wireos-vector-runtime.service"
    status
    ;;
  logs)
    remote "cd '$SERVER_RUNTIME' && docker compose logs --tail=200 vector-web-ui"
    ;;
  mcp-test)
    mcp_test
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
