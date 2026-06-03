import { join, relative } from "path";
import { readdir, readFile, stat } from "fs/promises";
import dgram from "dgram";
import { networkInterfaces } from "os";
import net from "net";

let robotStreamSocket = null;

const PORT = Number(Bun.env.PORT || 3000);
const PUBLIC_DIR = join(import.meta.dir, "public");
const ANIMATIONS_ROOT = Bun.env.VECTOR_ANIMATIONS_ROOT || "/tmp/vector-animations-build/assets";

// ── Motor state ──────────────────────────────────────────────────────────────
// target  = what the browser requested
// current = what we're actually sending (ramped toward target)
let robotIp = Bun.env.VECTOR_ROBOT_IP || "192.168.1.89";
let robotPort = Number(Bun.env.VECTOR_ROBOT_PORT || 8080);

function robotUrl(path, ip = robotIp, port = robotPort) {
  return `http://${ip}:${port}${path}`;
}

function getLocalIpForRobot(robotIp) {
  const nets = networkInterfaces();
  const robotSubnet = robotIp.split('.').slice(0, 3).join('.');
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        if (net.address.startsWith(robotSubnet)) {
          return net.address;
        }
      }
    }
  }
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        return net.address;
      }
    }
  }
  return "127.0.0.1";
}

let udpSocket = null;
const smoothedEnergies = [0, 0, 0, 0];
const alpha = 0.15;
let currentBestChannel = 0;
let channelSwitchCounter = 0;

function startUdpAudio() {
  if (udpSocket) {
    try { udpSocket.close(); } catch(_) {}
    udpSocket = null;
  }
  udpSocket = dgram.createSocket("udp4");
  
  udpSocket.on("message", (msg) => {
    if (msg.length < 20) return;
    const magic = msg.readUInt32LE(0);
    if (magic !== 0x56415544) return; // "VAUD"
    const payloadLen = msg.readUInt32LE(16);
    if (msg.length < 20 + payloadLen) return;
    
    const audioData = msg.subarray(20, 20 + payloadLen);
    const numSamples = payloadLen / 2;
    const samplesPerChannel = numSamples / 4;
    if (samplesPerChannel <= 0) return;
    
    const channelEnergies = [0, 0, 0, 0];
    for (let i = 0; i < samplesPerChannel; i++) {
      for (let ch = 0; ch < 4; ch++) {
        const sampleIdx = i * 4 + ch;
        const val = audioData.readInt16LE(sampleIdx * 2);
        channelEnergies[ch] += val * val;
      }
    }
    
    for (let ch = 0; ch < 4; ch++) {
      const rms = Math.sqrt(channelEnergies[ch] / samplesPerChannel);
      smoothedEnergies[ch] = alpha * rms + (1 - alpha) * smoothedEnergies[ch];
    }
    
    let bestCh = 0;
    let maxEnergy = smoothedEnergies[0];
    for (let ch = 1; ch < 4; ch++) {
      if (smoothedEnergies[ch] > maxEnergy) {
        maxEnergy = smoothedEnergies[ch];
        bestCh = ch;
      }
    }
    
    if (bestCh !== currentBestChannel) {
      channelSwitchCounter++;
      if (channelSwitchCounter >= 10) {
        currentBestChannel = bestCh;
        channelSwitchCounter = 0;
      }
    } else {
      channelSwitchCounter = 0;
    }
    
    const monoBuffer = Buffer.alloc(samplesPerChannel * 2);
    for (let i = 0; i < samplesPerChannel; i++) {
      const val = audioData.readInt16LE((i * 4 + currentBestChannel) * 2);
      monoBuffer.writeInt16LE(val, i * 2);
    }
    
    const base64Data = monoBuffer.toString("base64");
    const outbound = JSON.stringify({
      type: "audio_chunk",
      data: base64Data,
      energies: smoothedEnergies,
      selectedChannel: currentBestChannel
    });
    
    for (const ws of wsClients) {
      try { ws.send(outbound); } catch (_) {}
    }
  });

  udpSocket.on("error", (err) => {
    console.error("[UDP Socket Error]", err);
  });

  udpSocket.bind(5005, "0.0.0.0", () => {
    console.log("[UDP] Bound to 0.0.0.0:5005");
  });
}

function stopUdpAudio() {
  if (udpSocket) {
    try { udpSocket.close(); } catch (_) {}
    udpSocket = null;
    console.log("[UDP] Socket closed");
  }
}

// Periodic IMU polling from status endpoint
setInterval(async () => {
  if (wsClients.size === 0) return;
  try {
    const res = await fetch(robotUrl("/v1/status"));
    if (res.ok) {
      const data = await res.json();
      if (data.imu) {
        const msg = JSON.stringify({ type: "imu", data: data.imu });
        for (const ws of wsClients) {
          try { ws.send(msg); } catch (_) {}
        }
      }
    }
  } catch (_) {}
}, 200);

// Periodic motor encoder state push (100ms)
setInterval(async () => {
  if (wsClients.size === 0) return;
  try {
    const res = await fetch(robotUrl("/v1/motors/state"));
    if (res.ok) {
      const data = await res.json();
      const msg = JSON.stringify({ type: "motor_state", data });
      for (const ws of wsClients) {
        try { ws.send(msg); } catch (_) {}
      }
    }
  } catch (_) {}
}, 100);

const target  = { left: 0, right: 0, lift: 0, head: 0 };
const current = { left: 0, right: 0, lift: 0, head: 0 };

function resetMotorState() {
  target.left = target.right = target.lift = target.head = 0;
  current.left = current.right = current.lift = current.head = 0;
}

// Per-50ms-tick ramp limits (0→1 in ~250ms accel, 1→0 in ~140ms brake)
const ACCEL = 0.20;
const BRAKE = 0.35;

function ramp(cur, tgt) {
  if (cur === tgt) return cur;
  const rate = (tgt === 0 || Math.abs(tgt) < Math.abs(cur)) ? BRAKE : ACCEL;
  const diff = tgt - cur;
  return cur + Math.sign(diff) * Math.min(Math.abs(diff), rate);
}

const wsClients = new Set();

// ── Server-side motor loop (50 ms) ───────────────────────────────────────────
// Runs in Bun process — no browser timer jitter, no per-command TCP overhead.
// TTL=350ms gives 7× overlap so Spine MCU never power-gaps between ticks.
// NOTE: vector-hw-api now polls the Spine unconditionally at 50Hz in C++,
//       so this loop only needs to send commands when actually moving.
setInterval(async () => {
  if (wsClients.size === 0) return;

  let anyMoving = false;
  for (const k of ["left", "right", "lift", "head"]) {
    current[k] = ramp(current[k], target[k]);
    if (Math.abs(current[k]) < 0.005) current[k] = 0;
    else anyMoving = true;
  }

  // Only send motor commands when there is actual movement.
  // The C++ daemon handles idle Spine polling independently.
  if (!anyMoving) return;

  try {
    const { left, right, lift, head } = current;
    await fetch(robotUrl("/v1/motors"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ left, right, lift, head, ttl_ms: 350 }),
    });
  } catch (_) {}
}, 50);

// ── Telemetry relay ──────────────────────────────────────────────────────────
// Runs as an independent forever-loop. Reconnects automatically.
// When robotIp changes we abort() the current fetch so it restarts with new IP.
let telemetryAbort = new AbortController();

async function runTelemetryRelay() {
  while (true) {
    try {
      const res = await fetch(robotUrl("/v1/events"), {
        signal: telemetryAbort.signal,
      });
      if (!res.body) { await sleep(1000); continue; }

      const reader = res.body.getReader();
      const dec    = new TextDecoder();
      let   buf    = "";

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        const parts = buf.split("\n\n");
        buf = parts.pop();
        for (const part of parts) {
          const line = part.split("\n").find(l => l.startsWith("data:"));
          if (!line) continue;
          try {
            const msg = JSON.stringify({ type: "telemetry", data: JSON.parse(line.slice(5).trim()) });
            for (const ws of wsClients) {
              try { ws.send(msg); } catch (_) {}
            }
          } catch (_) {}
        }
      }
    } catch (e) {
      if (e?.name === "AbortError") {
        // IP changed — reset abort controller and reconnect
        telemetryAbort = new AbortController();
      }
      await sleep(1000);
    }
  }
}
runTelemetryRelay();

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ── MIME types ───────────────────────────────────────────────────────────────
const MIME = {
  html: "text/html; charset=utf-8",
  css:  "text/css; charset=utf-8",
  js:   "application/javascript; charset=utf-8",
  json: "application/json",
  png:  "image/png",
  jpg:  "image/jpeg",
  svg:  "image/svg+xml",
};

// ── Bun HTTP + WebSocket server ───────────────────────────────────────────────
const server = Bun.serve({
  port: PORT,

  fetch(req, server) {
    const url = new URL(req.url);

    if (url.pathname === "/ws") {
      if (server.upgrade(req)) return undefined;
      return new Response("WS upgrade failed", { status: 400 });
    }

    if (url.pathname === "/favicon.ico") return new Response(null, { status: 204 });

    if (url.pathname === "/api/config") {
      return new Response(JSON.stringify({ ip: robotIp, port: robotPort }), {
        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
      });
    }

    if (url.pathname === "/local/animations" && req.method === "GET") return listAnimations();
    if (url.pathname === "/local/animations/play" && req.method === "POST") return playAnimation(req);
    if (url.pathname === "/local/animations/stop" && req.method === "POST") return stopAnimation(req);

    if (url.pathname === "/api/camera/stream") return cameraStreamProxy(req, url);

    if (url.pathname.startsWith("/api/")) return apiProxy(req, url);

    return serveFile(url.pathname);
  },

  websocket: {
    open(ws) {
      wsClients.add(ws);
      resetMotorState();
      robotPost("/v1/motors/stop", {});
      ws.send(JSON.stringify({ type: "config", ip: robotIp, port: robotPort }));
      console.log(`[WS] +client  total=${wsClients.size}`);
    },

    message(ws, raw) {
      if (typeof raw !== "string") {
        // High-speed binary frame stream pass-through
        if (robotStreamSocket && !robotStreamSocket.destroyed) {
          robotStreamSocket.write(raw);
        }
        return;
      }

      let msg;
      try { msg = JSON.parse(raw); } catch { return; }

      switch (msg.type) {
        case "display_stream_start":
          if (robotStreamSocket) {
            robotStreamSocket.destroy();
            robotStreamSocket = null;
          }
          robotStreamSocket = net.connect({ host: robotIp, port: robotPort }, () => {
            robotStreamSocket.write("POST /v1/display/stream HTTP/1.1\r\n" +
                                    "Host: " + robotIp + ":" + robotPort + "\r\n" +
                                    "Content-Type: application/octet-stream\r\n" +
                                    "Connection: keep-alive\r\n\r\n");
          });
          break;

        case "display_stream_stop":
          if (robotStreamSocket) {
            robotStreamSocket.destroy();
            robotStreamSocket = null;
          }
          break;

        case "motors":
          target.left  = clamp(msg.left  ?? 0);
          target.right = clamp(msg.right ?? 0);
          target.lift  = clamp(msg.lift  ?? 0);
          target.head  = clamp(msg.head  ?? 0);
          break;

        case "leds":
          robotPost("/v1/leds/backpack", msg.leds);
          break;

        case "brightness":
          robotPost("/v1/display/brightness", { level: msg.level });
          break;

        case "motor_position":
          robotPost("/v1/motors/position", {
            motor: msg.motor,
            ticks: msg.ticks,
            power: msg.power ?? 0.5,
          });
          break;

        case "motor_stop":
          robotPost("/v1/motors/stop", {});
          break;

        case "camera_daemon_start":
          robotPost("/v1/camera/daemon/start", {});
          break;

        case "camera_daemon_stop":
          robotPost("/v1/camera/daemon/stop", {});
          break;

        case "config":
          if (msg.ip && msg.ip !== robotIp) {
            robotIp = msg.ip;
            // Restart telemetry relay with new IP
            telemetryAbort.abort();
          }
          if (msg.port && Number(msg.port) !== robotPort) {
            robotPort = Number(msg.port);
            telemetryAbort.abort();
          }
          break;

        case "mic_start": {
          startUdpAudio();
          const localIp = getLocalIpForRobot(robotIp);
          console.log(`[MIC] Starting stream from ${robotIp} to local IP ${localIp}:5005`);
          robotPost("/v1/audio/stream/start", { ip: localIp, port: 5005 });
          break;
        }

        case "mic_stop": {
          stopUdpAudio();
          console.log(`[MIC] Stopping stream from ${robotIp}`);
          robotPost("/v1/audio/stop", {});
          break;
        }
      }
    },

    close(ws) {
      wsClients.delete(ws);
      resetMotorState();
      robotPost("/v1/motors/stop", {});
      if (wsClients.size === 0) {
        stopUdpAudio();
        robotPost("/v1/audio/stop", {});
        if (robotStreamSocket) {
          robotStreamSocket.destroy();
          robotStreamSocket = null;
        }
      }
      console.log(`[WS] -client  total=${wsClients.size}`);
    },
  },
});

// ── Helpers ──────────────────────────────────────────────────────────────────
const clamp = v => Math.max(-1, Math.min(1, v));

function robotPost(path, body) {
  fetch(robotUrl(path), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  }).catch(() => {});
}

async function walkFiles(dir, out = []) {
  let entries = [];
  try {
    entries = await readdir(dir, { withFileTypes: true });
  } catch (_) {
    return out;
  }
  for (const ent of entries) {
    const p = join(dir, ent.name);
    if (ent.isDirectory()) await walkFiles(p, out);
    else if (ent.isFile() && ent.name.endsWith(".json")) out.push(p);
  }
  return out;
}

function summarizeAnimationFrames(name, relPath, frames) {
  const tracks = {};
  let durationMs = 0;
  for (const frame of frames) {
    const track = frame?.Name || "Unknown";
    tracks[track] = (tracks[track] || 0) + 1;
    const t = Number(frame?.triggerTime_ms || 0);
    const d = Number(frame?.durationTime_ms || 0);
    durationMs = Math.max(durationMs, t + d);
  }
  const parts = relPath.split("/");
  return {
    type: "clip",
    name,
    path: relPath,
    group: parts.length > 2 ? parts[1] : "",
    duration_ms: durationMs,
    frame_count: frames.length,
    tracks,
  };
}

function summarizeAnimationGroup(name, relPath, entries) {
  const parts = relPath.split("/");
  const totalWeight = entries.reduce((acc, entry) => acc + Number(entry?.Weight || 0), 0);
  const clipNames = entries.map(entry => String(entry?.Name || "")).filter(Boolean);
  return {
    type: "group",
    name,
    path: relPath,
    group: parts.length > 2 ? parts[1] : "",
    clip_count: clipNames.length,
    total_weight: totalWeight,
    clips: clipNames.slice(0, 12),
  };
}

async function animationIndex() {
  const root = join(ANIMATIONS_ROOT, "animations");
  const files = await walkFiles(root);
  const items = [];
  for (const file of files) {
    try {
      const data = JSON.parse(await readFile(file, "utf8"));
      const name = Object.keys(data)[0];
      const frames = Array.isArray(data[name]) ? data[name] : [];
      if (!name || frames.length === 0) continue;
      items.push({
        ...summarizeAnimationFrames(name, relative(ANIMATIONS_ROOT, file), frames),
        _file: file,
      });
    } catch (_) {}
  }
  items.sort((a, b) => a.name.localeCompare(b.name));
  return items;
}

async function animationGroupIndex() {
  const root = join(ANIMATIONS_ROOT, "animationGroups");
  const files = await walkFiles(root);
  const items = [];
  for (const file of files) {
    try {
      const data = JSON.parse(await readFile(file, "utf8"));
      const entries = Array.isArray(data.Animations) ? data.Animations : [];
      if (entries.length === 0) continue;
      const relPath = relative(ANIMATIONS_ROOT, file);
      const fileName = file.split("/").pop() || "";
      const name = fileName.replace(/\.json$/i, "");
      items.push({
        ...summarizeAnimationGroup(name, relPath, entries),
        _file: file,
      });
    } catch (_) {}
  }
  items.sort((a, b) => a.name.localeCompare(b.name));
  return items;
}

async function listAnimations() {
  try {
    const rootStat = await stat(ANIMATIONS_ROOT).catch(() => null);
    if (!rootStat?.isDirectory()) {
      return jsonResponse({ root: ANIMATIONS_ROOT, animations: [], error: "animations root not found" }, 404);
    }
    const animations = (await animationIndex()).map(({ _file, ...item }) => item);
    const groups = (await animationGroupIndex()).map(({ _file, ...item }) => item);
    return jsonResponse({ root: ANIMATIONS_ROOT, animations, groups });
  } catch (e) {
    return jsonResponse({ error: e.message, root: ANIMATIONS_ROOT }, 500);
  }
}

async function resolveAnimationSelection(name, requestedType = "") {
  const animations = await animationIndex();
  const byName = new Map(animations.map(item => [item.name, item]));
  if (requestedType !== "group") {
    const item = animations.find(a => a.name === name || a.path === name);
    if (item) return { item, selectedGroup: null, groupChoice: null };
  }

  const groups = await animationGroupIndex();
  const group = groups.find(g => g.name === name || g.path === name);
  if (!group) return null;
  const data = JSON.parse(await readFile(group._file, "utf8"));
  const entries = (Array.isArray(data.Animations) ? data.Animations : [])
    .filter(entry => byName.has(String(entry?.Name || "")));
  if (entries.length === 0) {
    return { error: "group has no available animation clips", group };
  }
  entries.sort((a, b) => {
    const weightDelta = Number(b.Weight || 0) - Number(a.Weight || 0);
    if (weightDelta) return weightDelta;
    return String(a.Name || "").localeCompare(String(b.Name || ""));
  });
  const choice = entries[0];
  return {
    item: byName.get(String(choice.Name)),
    selectedGroup: group,
    groupChoice: choice,
  };
}

function compactFrame(frame) {
  const name = frame?.Name;
  const base = {
    n: name,
    t: Number(frame?.triggerTime_ms || 0),
    d: Number(frame?.durationTime_ms || 0),
  };
  if (name === "LiftHeightKeyFrame") return { ...base, h: Number(frame.height_mm || 0) };
  if (name === "HeadAngleKeyFrame") return { ...base, a: Number(frame.angle_deg || 0) };
  if (name === "BodyMotionKeyFrame") return { ...base, r: String(frame.radius_mm ?? "STRAIGHT"), s: Number(frame.speed || 0) };
  if (name === "BackpackLightsKeyFrame") {
    return { ...base, f: frame.Front || frame.Left || [], m: frame.Middle || [], b: frame.Back || frame.Right || [] };
  }
  if (name === "ProceduralFaceKeyFrame") {
    const left = Array.isArray(frame.leftEye) ? frame.leftEye.slice(0, 4).map(Number) : [];
    const right = Array.isArray(frame.rightEye) ? frame.rightEye.slice(0, 4).map(Number) : [];
    return { ...base, l: left, e: right, sx: Number(frame.faceScaleX ?? 1), sy: Number(frame.faceScaleY ?? 1), cx: Number(frame.faceCenterX ?? 0), cy: Number(frame.faceCenterY ?? 0) };
  }
  if (name === "FaceAnimationKeyFrame") return { ...base, anim: String(frame.animName || "") };
  if (name === "RobotAudioKeyFrame") return { ...base, audio: true };
  if (name === "EventKeyFrame") return { ...base, event: String(frame.event_id || "") };
  return base;
}

function buildAnimationScript(animationName, events) {
  const eventsJson = JSON.stringify(events);
  const nameJson = JSON.stringify(animationName);
  return String.raw`
from vector_robot import VectorRobot
import json
import math
import time

EVENTS = json.loads(${JSON.stringify(eventsJson)})
ANIMATION_NAME = ${nameJson}
FACE_W = 184
FACE_H = 96
LIFT_ZERO_RAW = -4
LIFT_TICKS_PER_MM = 190.0 / 92.0
HEAD_MIN_DEG = -22.0
HEAD_ZERO_RAW = -320
HEAD_TICKS_PER_DEG = 554.0 / 67.0
TRACK_TICKS_PER_MM = 8.0

robot = VectorRobot()

def clamp(v, lo, hi):
    return max(lo, min(hi, v))

def post_bytes(path, payload, content_type="application/octet-stream"):
    return robot.request("POST", path, payload, content_type=content_type)

def motors():
    return robot.motors_state().get("motors") or []

def motor_pos(i):
    ms = motors()
    if len(ms) <= i:
        return 0
    return int(ms[i].get("position", 0))

def post_json(path, body):
    return robot.request("POST", path, body)

def move_abs(motor, target, power, timeout_ms, tolerance=8):
    cur = motor_pos(motor)
    delta = int(round(target - cur))
    if abs(delta) <= tolerance:
        return
    # /v1/motors/position starts a robot-side profiled move and returns
    # immediately. Do not block the animation scheduler waiting for the joint
    # to settle; later keyframes may update the target.
    post_json("/v1/motors/position", {
        "motor": int(motor),
        "ticks": int(delta),
        "power": float(power),
        "min_power": 0.08 if motor in (2, 3) else 0.25,
        "tolerance": int(tolerance),
        "timeout_ms": max(350, int(timeout_ms)),
    })

def lift_target_raw(height_mm):
    return LIFT_ZERO_RAW + clamp(float(height_mm), 0.0, 92.0) * LIFT_TICKS_PER_MM

def head_target_raw(angle_deg):
    angle = clamp(float(angle_deg), HEAD_MIN_DEG, 45.0)
    return HEAD_ZERO_RAW + (angle - HEAD_MIN_DEG) * HEAD_TICKS_PER_DEG

def color3(vals):
    vals = list(vals or [])
    while len(vals) < 3:
        vals.append(0.0)
    if max(vals[:3] or [0]) <= 1.0:
        vals = [v * 255.0 for v in vals]
    return [int(clamp(round(v), 0, 255)) for v in vals[:3]]

def set_backpack(ev):
    front = color3(ev.get("f"))
    middle = color3(ev.get("m"))
    back = color3(ev.get("b"))
    # DDL has Front/Middle/Back groups; Vector hardware exposes four RGB LEDs (LED 0=back, 1=middle, 2=front, 3=status).
    robot.request("POST", "/v1/leds/backpack", {"led": 0, "r": back[0], "g": back[1], "b": back[2]})
    robot.request("POST", "/v1/leds/backpack", {"led": 1, "r": middle[0], "g": middle[1], "b": middle[2]})
    robot.request("POST", "/v1/leds/backpack", {"led": 2, "r": front[0], "g": front[1], "b": front[2]})

def rgb565(r, g, b):
    v = ((int(r) >> 3) << 11) | ((int(g) >> 2) << 5) | (int(b) >> 3)
    return v & 255, (v >> 8) & 255

def fill_ellipse(buf, cx, cy, rx, ry, color):
    if rx <= 0 or ry <= 0:
        return
    lo = rgb565(*color)
    x0 = max(0, int(cx - rx - 1))
    x1 = min(FACE_W - 1, int(cx + rx + 1))
    y0 = max(0, int(cy - ry - 1))
    y1 = min(FACE_H - 1, int(cy + ry + 1))
    for y in range(y0, y1 + 1):
        dy = (y - cy) / ry
        for x in range(x0, x1 + 1):
            dx = (x - cx) / rx
            if dx * dx + dy * dy <= 1.0:
                idx = (y * FACE_W + x) * 2
                buf[idx] = lo[0]
                buf[idx + 1] = lo[1]

def draw_eye(buf, eye, face_cx, face_cy, sx, sy):
    ex = float(eye[0]) if len(eye) > 0 else 0.0
    ey = float(eye[1]) if len(eye) > 1 else 0.0
    ew = float(eye[2]) if len(eye) > 2 else 1.5
    eh = float(eye[3]) if len(eye) > 3 else 1.1
    cx = FACE_W / 2 + face_cx + ex * 4.0 * sx
    cy = FACE_H / 2 + face_cy + ey * 3.0 * sy
    rx = clamp(abs(ew) * 12.0 * sx, 5.0, 34.0)
    ry = clamp(abs(eh) * 12.0 * sy, 4.0, 28.0)
    fill_ellipse(buf, cx, cy, rx + 2, ry + 2, (0, 32, 42))
    fill_ellipse(buf, cx, cy, rx, ry, (0, 238, 255))
    fill_ellipse(buf, cx + rx * 0.25, cy - ry * 0.25, max(2, rx * 0.18), max(2, ry * 0.18), (210, 255, 255))

def render_face(ev):
    buf = bytearray(FACE_W * FACE_H * 2)
    sx = clamp(float(ev.get("sx", 1.0)), 0.4, 1.8)
    sy = clamp(float(ev.get("sy", 1.0)), 0.4, 1.8)
    face_cx = clamp(float(ev.get("cx", 0.0)), -45.0, 45.0)
    face_cy = clamp(float(ev.get("cy", 0.0)), -25.0, 25.0)
    draw_eye(buf, ev.get("l") or [], face_cx, face_cy, sx, sy)
    draw_eye(buf, ev.get("e") or [], face_cx, face_cy, sx, sy)
    post_bytes("/v1/display/frame", bytes(buf))

def body_motion(ev):
    dur = max(40, int(ev.get("d") or 80))
    speed = float(ev.get("s") or 0.0)
    radius = str(ev.get("r") or "STRAIGHT").upper()
    if abs(speed) < 1:
        robot.set_motors(0, 0, ttl_ms=dur + 120)
        return
    if radius == "TURN_IN_PLACE":
        p = clamp(speed / 350.0, -0.55, 0.55)
        robot.set_motors(left=-p, right=p, ttl_ms=dur + 120)
    elif radius == "STRAIGHT":
        ticks = int(round(speed * (dur / 1000.0) * TRACK_TICKS_PER_MM))
        if abs(ticks) >= 8:
            post_json("/v1/motors/drive", {
                "ticks": ticks,
                "power": min(0.45, max(0.25, abs(speed) / 350.0)),
                "min_power": 0.25,
                "tolerance": 12,
                "timeout_ms": dur + 650,
            })
        else:
            p = clamp(speed / 350.0, -0.55, 0.55)
            robot.set_motors(left=p, right=p, ttl_ms=dur + 120)
    else:
        try:
            r = float(radius)
        except Exception:
            r = 9999.0
        half = 27.0
        if abs(r) < 1.0:
            p = clamp(speed / 350.0, -0.55, 0.55)
            robot.set_motors(left=-p, right=p, ttl_ms=dur + 120)
        else:
            left_speed = speed * (r - half) / r
            right_speed = speed * (r + half) / r
            max_abs = max(abs(left_speed), abs(right_speed), 1.0)
            scale = min(0.55, abs(speed) / 350.0) / max_abs
            robot.set_motors(left=left_speed * scale, right=right_speed * scale, ttl_ms=dur + 120)

def handle(ev):
    n = ev.get("n")
    d = max(250, int(ev.get("d") or 250) + 500)
    if n == "LiftHeightKeyFrame":
        move_abs(2, lift_target_raw(ev.get("h", 0)), 0.35, d, 10)
    elif n == "HeadAngleKeyFrame":
        move_abs(3, head_target_raw(ev.get("a", 0)), 0.30, d, 10)
    elif n == "BackpackLightsKeyFrame":
        set_backpack(ev)
    elif n == "ProceduralFaceKeyFrame":
        render_face(ev)
    elif n == "BodyMotionKeyFrame":
        body_motion(ev)
    elif n in ("FaceAnimationKeyFrame", "RobotAudioKeyFrame", "EventKeyFrame", "RecordHeadingKeyFrame", "TurnToRecordedHeadingKeyFrame"):
        print("unsupported_track", n, ev.get("anim") or ev.get("event") or "")

def main():
    print("animation_start", ANIMATION_NAME, "events", len(EVENTS))
    robot.request("POST", "/v1/display/init", {})
    start = time.monotonic()
    for ev in sorted(EVENTS, key=lambda x: int(x.get("t") or 0)):
        target = start + (int(ev.get("t") or 0) / 1000.0)
        delay = target - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        handle(ev)
    end_ms = max([int(e.get("t") or 0) + int(e.get("d") or 0) for e in EVENTS] or [0])
    remain = start + end_ms / 1000.0 - time.monotonic()
    if remain > 0:
        time.sleep(min(remain, 2.0))
    robot.stop_motors()
    print("animation_done", ANIMATION_NAME)

try:
    main()
finally:
    try:
        robot.stop_motors()
    except Exception:
        pass
`;
}

async function playAnimation(req) {
  try {
    const ip = req.headers.get("x-robot-ip") || robotIp;
    const port = Number(req.headers.get("x-robot-port") || robotPort);
    const body = await req.json();
    const name = String(body.name || "").trim();
    if (!name) return jsonResponse({ error: "name required" }, 400);
    const resolved = await resolveAnimationSelection(name, String(body.type || ""));
    if (!resolved) return jsonResponse({ error: "animation not found", name }, 404);
    if (resolved.error) return jsonResponse({ error: resolved.error, name }, 404);
    const { item, selectedGroup, groupChoice } = resolved;
    const data = JSON.parse(await readFile(item._file, "utf8"));
    const animName = Object.keys(data)[0];
    const frames = Array.isArray(data[animName]) ? data[animName] : [];
    const events = frames.map(compactFrame).filter(e => e.n);
    const script = buildAnimationScript(animName, events);
    const prefix = selectedGroup
      ? `animation_group ${selectedGroup.name} selected ${item.name} weight ${Number(groupChoice?.Weight || 0)}\n`
      : "";
    const upstream = await fetch(robotUrl("/v1/apps/run-script", ip, port), {
      method: "POST",
      headers: { "Content-Type": "text/x-python" },
      body: script,
    });
    const headers = new Headers(upstream.headers);
    headers.set("Access-Control-Allow-Origin", "*");
    if (!prefix || !upstream.body) {
      return new Response(upstream.body, { status: upstream.status, headers });
    }
    const prefixed = new ReadableStream({
      start(controller) {
        controller.enqueue(new TextEncoder().encode(prefix));
        const reader = upstream.body.getReader();
        function pump() {
          reader.read().then(({ done, value }) => {
            if (done) {
              controller.close();
              return;
            }
            controller.enqueue(value);
            pump();
          }).catch(error => controller.error(error));
        }
        pump();
      },
    });
    return new Response(prefixed, { status: upstream.status, headers });
  } catch (e) {
    return jsonResponse({ error: e.message }, 500);
  }
}

async function stopAnimation(req) {
  const ip = req.headers.get("x-robot-ip") || robotIp;
  const port = Number(req.headers.get("x-robot-port") || robotPort);
  await Promise.allSettled([
    fetch(robotUrl("/v1/motors/stop", ip, port), { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" }),
    fetch(robotUrl("/v1/audio/stop", ip, port), { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" }),
  ]);
  return jsonResponse({ ok: true });
}

function jsonResponse(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
  });
}

async function apiProxy(req, url) {
  const ip = req.headers.get("x-robot-ip") || robotIp;
  const port = Number(req.headers.get("x-robot-port") || robotPort);
  const target = robotUrl(`${url.pathname.replace("/api/", "/v1/")}${url.search}`, ip, port);
  if (req.method === "OPTIONS") {
    return new Response(null, { headers: {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type, x-robot-ip, x-robot-port",
    }});
  }
  try {
    const h = new Headers(req.headers);
    h.delete("host");
    h.delete("x-robot-ip");
    h.delete("x-robot-port");
    h.delete("content-length");
    h.delete("transfer-encoding");
    const opts = { method: req.method, headers: h };
    if (req.method !== "GET" && req.method !== "HEAD") {
      const body = new Uint8Array(await req.arrayBuffer());
      opts.body = body;
      h.set("Content-Length", String(body.byteLength));
    }
    const r  = await fetch(target, opts);
    const rh = new Headers(r.headers);
    rh.set("Access-Control-Allow-Origin", "*");
    return new Response(r.body, { status: r.status, headers: rh });
  } catch (e) {
    return new Response(JSON.stringify({ error: e.message }), {
      status: 502,
      headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
    });
  }
}

// ── Camera multipart stream proxy ─────────────────────────────────────────────
// Pass-through: connects to robot camera stream and forwards multipart frames.
async function cameraStreamProxy(req, url) {
  const ip = req.headers.get("x-robot-ip") || robotIp;
  const port = Number(req.headers.get("x-robot-port") || robotPort);
  try {
    const upstream = await fetch(robotUrl(`/v1/camera/stream${url.search}`, ip, port), {
      headers: { Accept: "multipart/x-mixed-replace" },
    });
    if (!upstream.ok || !upstream.body) {
      return new Response(JSON.stringify({ error: "camera stream unavailable" }), {
        status: 503,
        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
      });
    }
    const ct = upstream.headers.get("content-type") || "multipart/x-mixed-replace; boundary=frame";
    return new Response(upstream.body, {
      status: 200,
      headers: {
        "Content-Type": ct,
        "Cache-Control": "no-cache",
        "Access-Control-Allow-Origin": "*",
      },
    });
  } catch (e) {
    return new Response(JSON.stringify({ error: e.message }), {
      status: 502,
      headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
    });
  }
}

async function serveFile(pathname) {
  const path = pathname === "/" ? "/index.html" : pathname;
  const file = Bun.file(join(PUBLIC_DIR, path));
  if (await file.exists()) {
    const ext = path.split(".").pop();
    return new Response(file, {
      headers: { "Content-Type": MIME[ext] || "application/octet-stream" },
    });
  }
  return new Response("Not Found", { status: 404 });
}

console.log(`\n🤖  Vector Web UI  →  http://localhost:${PORT}\n`);
console.log(`Default robot API: ${robotIp}:${robotPort}`);
