import { join } from "path";
import dgram from "dgram";
import { networkInterfaces } from "os";

const PORT = Number(Bun.env.PORT || 3000);
const PUBLIC_DIR = join(import.meta.dir, "public");

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

    if (url.pathname === "/api/camera/stream") return cameraStreamProxy(req, url);

    if (url.pathname.startsWith("/api/")) return apiProxy(req, url);

    return serveFile(url.pathname);
  },

  websocket: {
    open(ws) {
      wsClients.add(ws);
      ws.send(JSON.stringify({ type: "config", ip: robotIp, port: robotPort }));
      console.log(`[WS] +client  total=${wsClients.size}`);
    },

    message(ws, raw) {
      let msg;
      try { msg = JSON.parse(raw); } catch { return; }

      switch (msg.type) {
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
      if (wsClients.size === 0) {
        // Safety: zero target so ramp brings motors to 0
        target.left = target.right = target.lift = target.head = 0;
        stopUdpAudio();
        robotPost("/v1/audio/stop", {});
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
