/* ════════════════════════════════════════════════════════════
   VECTOR CONSOLE — app.js
   Motor state is sent to server via WebSocket.
   Server ramps to target and fires at robot every 50ms.
   ════════════════════════════════════════════════════════════ */

// ── State ──────────────────────────────────────────────────────────────────
const S = {
  ws:        null,
  connected: false,
  telemetryWatchdog: null,
  // Local target motor state (server does the actual ramping)
  motors: { left: 0, right: 0, lift: 0, head: 0 },
  // Telemetry heartbeat
  lastTelemetry: 0,
  telemetryAlive: false,
};

// ── DOM refs ───────────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const D = {
  ip:       $("robot-ip"),
  btnConn:  $("btn-connect"),
  dot:      $("dot"),
  label:    $("conn-label"),
  batFill:  $("bat-fill"),
  batV:     $("bat-v"),
  batT:     $("bat-t"),
  tBeat:    $("telemetry-beat"),
  // joystick
  jZone:    $("joystick-zone"),
  jKnob:    $("joystick-knob"),
  jState:   $("js-state"),
  vml:      $("vml"),
  vmr:      $("vmr"),
  // sliders
  sHead:    $("slider-head"),
  vHead:    $("vhead"),
  sLift:    $("slider-lift"),
  vLift:    $("vlift"),
  sBright:  $("slider-bright"),
  vBright:  $("vbright"),
  // leds
  ledColor: $("led-color"),
  ledPrev:  $("led-preview"),
  facePreview: $("face-preview"),
  displayFile: $("display-file"),
  displayInit: $("btn-display-init"),
  displaySend: $("btn-display-send"),
  videoToggle: $("btn-video-toggle"),
  videoSource: $("video-source"),
  vDisplay: $("vdisplay"),
  audioFile: $("audio-file"),
  sVolume: $("slider-volume"),
  vVolume: $("vvolume"),
  audioPlay: $("btn-audio-play"),
  audioStop: $("btn-audio-stop"),
  vAudio: $("vaudio"),
  // sensors
  driveCanvas: $("drive-canvas"),
  cfl: $("cfl"), cfr: $("cfr"), cbl: $("cbl"), cbr: $("cbr"),
  vfl: $("vfl"), vfr: $("vfr"), vbl: $("vbl"), vbr: $("vbr"),
  vax: $("vax"), vay: $("vay"), vaz: $("vaz"),
  vgx: $("vgx"), vgy: $("vgy"), vgz: $("vgz"),
  vt0: $("vt0"), vfc: $("vfc"),
  vproxDist: $("vprox-dist"), vproxStatus: $("vprox-status"),
  btnMicToggle: $("btn-mic-toggle"),
  btnMicRecord: $("btn-mic-record"),
  vmicStatus: $("vmic-status"),
  beamArrow: $("beam-arrow"),
  micNodes: [
    $("mic-node-0"),
    $("mic-node-1"),
    $("mic-node-2"),
    $("mic-node-3")
  ],
  // encoder state
  encPos:   [0,1,2,3].map(i => $(`enc-pos-${i}`)),
  encDelta: [0,1,2,3].map(i => $(`enc-delta-${i}`)),
  encMoving:[0,1,2,3].map(i => $(`enc-moving-${i}`)),
  // position control
  gotoLiftTicks: $("goto-lift-ticks"),
  gotoHeadTicks: $("goto-head-ticks"),
  btnGotoLift: $("btn-goto-lift"),
  btnGotoHead: $("btn-goto-head"),
  btnHoldLift: $("btn-hold-lift"),
  btnHoldHead: $("btn-hold-head"),
  btnMotorsStop: $("btn-motors-stop"),
  // camera
  camFeedWrap: $("cam-feed-wrap"),
  camOffline:  $("cam-offline"),
  vcamDaemon:  $("vcam-daemon"),
  vcamStream:  $("vcam-stream"),
  vcamFps:     $("vcam-fps"),
  btnCamStart: $("btn-cam-start"),
  btnCamStop:  $("btn-cam-stop"),
  btnCamStreamOn: $("btn-cam-stream-on"),
  btnCamSnapshot: $("btn-cam-snapshot"),
  log: $("log"),
};

// ── Logging ────────────────────────────────────────────────────────────────
function log(tag, msg) {
  const t = new Date().toTimeString().slice(0, 8);
  const line = document.createElement("div");
  line.className = "log-line";
  line.innerHTML =
    `<span class="log-time">${t}</span>` +
    `<span class="log-tag ${tag}">${tag}</span>` +
    `<span>${msg}</span>`;
  D.log.appendChild(line);
  D.log.scrollTop = D.log.scrollHeight;
}

// ── WebSocket ──────────────────────────────────────────────────────────────
function toggleConnect() {
  if (S.ws) { disconnect(); } else { connect(); }
}

function connect() {
  const ip = D.ip.value.trim();
  if (!ip) { log("ERROR", "Enter robot IP first."); return; }
  localStorage.setItem("vec-ip", ip);

  setStatus("connecting");
  log("INFO", `Connecting to robot ${ip}…`);

  S.ws = new WebSocket(`ws://${location.host}/ws`);

  S.ws.onopen = () => {
    S.ws.send(JSON.stringify({ type: "config", ip }));
    S.connected = true;
    setStatus("online");
    D.btnConn.textContent = "DISCONNECT";
    D.btnConn.className = "btn danger";
    log("OK", "WebSocket connected — motor control active.");
    startTelemetryWatchdog();
  };

  S.ws.onmessage = ev => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (msg.type === "config" && msg.ip && !localStorage.getItem("vec-ip")) {
      D.ip.value = msg.ip;
      return;
    }
    if (msg.type === "telemetry") {
      S.lastTelemetry = Date.now();
      onTelemetry(msg.data);
      return;
    }
    if (msg.type === "imu") {
      const d = msg.data;
      if (d.accel) {
        D.vax.textContent = d.accel.x.toFixed(3);
        D.vay.textContent = d.accel.y.toFixed(3);
        D.vaz.textContent = d.accel.z.toFixed(3);
      }
      if (d.gyro) {
        D.vgx.textContent = d.gyro.x.toFixed(2);
        D.vgy.textContent = d.gyro.y.toFixed(2);
        D.vgz.textContent = d.gyro.z.toFixed(2);
      }
      return;
    }
    if (msg.type === "audio_chunk") {
      if (!micStreamActive) return;
      
      const binaryString = atob(msg.data);
      const len = binaryString.length;
      const bytes = new Uint8Array(len);
      for (let i = 0; i < len; i++) {
        bytes[i] = binaryString.charCodeAt(i);
      }
      
      const int16Samples = new Int16Array(bytes.buffer);
      const float32Samples = new Float32Array(int16Samples.length);
      for (let i = 0; i < int16Samples.length; i++) {
        float32Samples[i] = int16Samples[i] / 32768.0;
      }
      
      playMicChunk(float32Samples);
      
      if (micRecordingActive) {
        recordedSamples.push(...float32Samples);
      }
      
      const energies = msg.energies || [0, 0, 0, 0];
      const selectedCh = msg.selectedChannel ?? 0;
      
      for (let i = 0; i < 4; i++) {
        const energy = energies[i];
        const active = i === selectedCh;
        const node = D.micNodes[i];
        if (node) {
          const scale = 1.0 + Math.min(0.5, energy / 1200);
          const glow = Math.min(20, energy / 50);
          node.style.transform = `scale(${scale.toFixed(2)})`;
          if (active) {
            node.classList.add("active");
            node.style.boxShadow = `0 0 ${glow.toFixed(1)}px rgba(57,255,110,0.8)`;
          } else {
            node.classList.remove("active");
            node.style.boxShadow = `0 0 ${glow.toFixed(1)}px rgba(0,240,255,0.4)`;
          }
        }
      }
      
      const angles = [-45, 45, 135, -135];
      if (D.beamArrow) {
        D.beamArrow.style.transform = `translate(-50%, -100%) rotate(${angles[selectedCh]}deg)`;
      }
      return;
    }

    if (msg.type === "motor_state") {
      onMotorState(msg.data);
      return;
    }
  };

  S.ws.onerror = () => log("ERROR", "WebSocket error.");

  S.ws.onclose = () => {
    S.connected = false;
    S.ws = null;
    setStatus("offline");
    D.btnConn.textContent = "CONNECT";
    D.btnConn.className = "btn primary";
    log("WARN", "Connection closed.");
  };
}

function disconnect() {
  if (S.ws) S.ws.close();
}

function setStatus(state) {
  const states = {
    connecting: { dot: "connecting", label: "CONNECTING" },
    online:     { dot: "online",     label: "ONLINE" },
    offline:    { dot: "",           label: "DISCONNECTED" },
  };
  const s = states[state];
  D.dot.className = "dot " + s.dot;
  D.label.textContent = s.label;
  D.btnConn.disabled = state === "connecting";
}

// ── Telemetry watchdog — shows if data is arriving independently of motors ──
function startTelemetryWatchdog() {
  if (S.telemetryWatchdog) return;
  S.telemetryWatchdog = setInterval(() => {
    const age = Date.now() - S.lastTelemetry;
    const alive = age < 800;
    if (alive !== S.telemetryAlive) {
      S.telemetryAlive = alive;
      if (D.tBeat) {
        D.tBeat.className = "beat-dot " + (alive ? "alive" : "dead");
        D.tBeat.title = alive
          ? `Telemetry streaming (${age}ms ago)`
          : "Telemetry stale";
      }
    }
  }, 500);
}

// ── Send helpers ───────────────────────────────────────────────────────────
function sendMotors() {
  if (!S.ws || S.ws.readyState !== WebSocket.OPEN) return;
  S.ws.send(JSON.stringify({ type: "motors", ...S.motors }));
}

function sendLeds() {
  if (!S.ws || S.ws.readyState !== WebSocket.OPEN) return;
  const hex = D.ledColor.value;
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16));
  S.ws.send(JSON.stringify({ type: "leds", leds: Array(4).fill({ r, g, b }) }));
  log("INFO", `LEDs → rgb(${r},${g},${b})`);
}

function sendLedsRgb(r, g, b) {
  if (!S.ws || S.ws.readyState !== WebSocket.OPEN) return;
  S.ws.send(JSON.stringify({ type: "leds", leds: Array(4).fill({ r, g, b }) }));
  log("INFO", `LEDs → rgb(${r},${g},${b})`);
}

// ── Telemetry handler ──────────────────────────────────────────────────────
// Calibrated known-broken sensors:
//   proximity (TOF): status is always 90-91 (hardware fault), range is garbage
//   touch[1]:        always 0 (not populated on this unit)
// Known-working:
//   touch[0]:        590 idle, rises to ~620-640 on touch
//   cliff[0-3]:      100-215 on flat surface, drops <80 over edge

const CLIFF_HAZARD = 90;   // below this = cliff/air detected
const TOUCH0_ACTIVE = 610; // above this = touched

function onTelemetry(d) {
  if (!d) return;

  // Frame counter
  if (d.framecounter != null) D.vfc.textContent = d.framecounter.toLocaleString();

  // Battery
  if (d.battery) {
    const rawV = d.battery.main_voltage_raw;
    // Calibration: raw 3000 ≈ 3.6V, raw 3400 ≈ 4.1V (measured from real data)
    const v = (rawV / 826).toFixed(2);
    let t = d.battery.temperature_raw;
    // Raw temp can be in tenths of degrees on some firmware versions
    if (t > 200) t = (t / 10).toFixed(1);
    D.batV.textContent = v + " V";
    D.batT.textContent = t + "°C";
    // SVG fill bar: 3.5V=empty → 4.1V=full, bar max width=22px
    const pct = Math.max(0, Math.min(22, ((parseFloat(v) - 3.5) / 0.6) * 22));
    D.batFill.setAttribute("width", pct.toFixed(1));
    D.batFill.setAttribute("fill",
      pct > 14 ? "#39ff6e" : pct > 7 ? "#ffb300" : "#ff2e4c");
  }

  // Cliff sensors — update every telemetry frame (independent of motors)
  if (d.cliff && d.cliff.length >= 4) {
    [
      { el: D.cfl, txt: D.vfl, v: d.cliff[0] },
      { el: D.cfr, txt: D.vfr, v: d.cliff[1] },
      { el: D.cbl, txt: D.vbl, v: d.cliff[2] },
      { el: D.cbr, txt: D.vbr, v: d.cliff[3] },
    ].forEach(s => {
      s.txt.textContent = s.v;
      s.el.classList.toggle("hazard", s.v < CLIFF_HAZARD);
    });
  }

  // Touch[0] — working sensor
  if (d.touch && d.touch.length > 0) {
    const t0 = d.touch[0];
    D.vt0.textContent = t0;
    const touched = t0 > TOUCH0_ACTIVE;
    D.vt0.style.color = touched ? "var(--green)" : "var(--cyan)";
    D.vt0.title = touched ? "TOUCHED" : "idle";
  }

  // Proximity (TOF)
  if (d.proximity) {
    const range = d.proximity.range_mm;
    const status = d.proximity.status;
    
    if (range === 8190 || range === 8191) {
      D.vproxDist.textContent = "Out of Range";
      D.vproxDist.style.color = "var(--cyan)";
    } else {
      D.vproxDist.textContent = range + " mm";
      D.vproxDist.style.color = "var(--green)";
    }
    
    const statuses = {
      0: "Valid",
      1: "Sigma Fail",
      2: "Signal Fail",
      3: "Min Range Fail",
      4: "Phase Fail",
      5: "Hardware Fail",
      255: "No Update"
    };
    const statusText = statuses[status] || ("Code " + status);
    D.vproxStatus.textContent = statusText;
    if (status === 0) {
      D.vproxStatus.style.color = "var(--green)";
    } else if (status === 4 || range === 8190 || range === 8191) {
      D.vproxStatus.style.color = "var(--cyan)";
    } else {
      D.vproxStatus.style.color = "var(--red)";
    }
  }

  // Motor encoders — body telemetry uses "motors"; older payloads used "motor".
  const motorTelemetry = Array.isArray(d.motors) ? d.motors : d.motor;
  if (motorTelemetry && motorTelemetry.length >= 4) {
    renderMotorState(motorTelemetry);
  }

  // Drive visualiser update (drawn in canvas loop using S.motors)
}

// ── Motor state (encoder) display ─────────────────────────────────────────────────
function onMotorState(data) {
  if (!data) return;
  const motors = Array.isArray(data.motors) ? data.motors : data.motor;
  if (!motors) return;
  renderMotorState(motors);
}

function renderMotorState(motors) {
  motors.forEach((m, i) => {
    if (Number.isFinite(m.position)) latestMotorPos[i] = m.position;
    if (D.encPos[i])   D.encPos[i].textContent   = m.position.toLocaleString();
    if (D.encDelta[i]) D.encDelta[i].textContent = `Δ${m.delta > 0 ? '+' : ''}${m.delta}`;
    if (D.encMoving[i]) {
      D.encMoving[i].classList.toggle('active', m.moving ?? m.delta !== 0);
    }
  });
}

// ── Position control ──────────────────────────────────────────────────────────────
const holdState = { lift: false, head: false };
const holdTargetPos = { lift: null, head: null };
const latestMotorPos = [null, null, null, null];

async function postMotorPosition(motor, ticks, power) {
  const r = await robotFetch("/motors/position", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ motor, ticks, power }),
  });
  if (!r.ok) throw new Error(await r.text());
  return r.json().catch(() => ({}));
}

async function postMotorHold(motor, enabled, target = null) {
  const payload = enabled
    ? { motor, enabled: 1, target, power: 0.7, deadband: 6 }
    : { motor, enabled: 0 };
  const r = await robotFetch("/motors/hold", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (!r.ok) throw new Error(await r.text());
  return r.json().catch(() => ({}));
}

async function stopMotors() {
  const r = await robotFetch("/motors/stop", { method: "POST" });
  if (!r.ok) throw new Error(await r.text());
}

function sendWs(obj) {
  if (!S.ws || S.ws.readyState !== WebSocket.OPEN) return;
  S.ws.send(JSON.stringify(obj));
}

async function gotoMotor(motor, ticksInput) {
  const ticks = parseInt(ticksInput.value);
  if (isNaN(ticks) || ticks === 0) {
    log("WARN", "Ticks must be non-zero.");
    return;
  }
  const names = ["left_track", "right_track", "lift", "head"];
  try {
    await postMotorPosition(motor, ticks, 0.6);
    log("INFO", `Motor ${names[motor]} -> ${ticks > 0 ? '+' : ''}${ticks} ticks`);
  } catch (e) {
    log("ERROR", `Motor ${names[motor]} move failed: ${e.message}`);
  }
}

async function toggleHold(motor, btnEl, label) {
  // motor: 2=lift, 3=head
  const key = motor === 2 ? "lift" : "head";
  const next = !holdState[key];
  if (next) {
    if (latestMotorPos[motor] === null) {
      log("WARN", `${label} hold needs live encoder state first.`);
      return;
    }
    try {
      holdTargetPos[key] = latestMotorPos[motor];
      await postMotorHold(motor, true, holdTargetPos[key]);
      holdState[key] = true;
      btnEl.textContent = `RELEASE`;
      btnEl.classList.add("active");
      log("INFO", `${label} hold target ${holdTargetPos[key].toLocaleString()} ticks.`);
    } catch (e) {
      holdTargetPos[key] = null;
      holdState[key] = false;
      log("ERROR", `${label} hold failed: ${e.message}`);
    }
  } else {
    try {
      await postMotorHold(motor, false);
    } catch (e) {
      log("WARN", `${label} hold release failed: ${e.message}`);
    } finally {
      holdTargetPos[key] = null;
      holdState[key] = false;
      btnEl.textContent = `HOLD`;
      btnEl.classList.remove("active");
      log("INFO", `${label} hold released.`);
    }
  }
}

// ── Joystick ───────────────────────────────────────────────────────────────────
(function setupJoystick() {
  const zone = D.jZone;
  const knob = D.jKnob;
  const MAX  = 80;
  let active = false;
  let cx = 0, cy = 0;

  zone.addEventListener("pointerdown", e => {
    active = true;
    zone.setPointerCapture(e.pointerId);
    zone.classList.add("active");
    D.jState.textContent = "DRIVING";
    D.jState.classList.add("active");
    const r = zone.getBoundingClientRect();
    cx = r.left + r.width / 2;
    cy = r.top  + r.height / 2;
    updateJoystick(e);
  });

  zone.addEventListener("pointermove", e => {
    if (!active) return;
    updateJoystick(e);
  });

  const release = () => {
    if (!active) return;
    active = false;
    zone.classList.remove("active");
    D.jState.textContent = "STANDBY";
    D.jState.classList.remove("active");
    knob.style.transform = "translate(0,0)";
    S.motors.left = S.motors.right = 0;
    D.vml.textContent = "0%";
    D.vmr.textContent = "0%";
    sendMotors();
  };

  zone.addEventListener("pointerup",     release);
  zone.addEventListener("pointercancel", release);

  function updateJoystick(e) {
    let dx = e.clientX - cx;
    let dy = e.clientY - cy;
    const dist = Math.hypot(dx, dy);
    if (dist > MAX) { dx = dx / dist * MAX; dy = dy / dist * MAX; }
    knob.style.transform = `translate(${dx}px,${dy}px)`;
    // Tank-drive mixing: ny=forward, nx=turn
    const nx =  dx / MAX;
    const ny = -dy / MAX;
    S.motors.left  = Math.max(-1, Math.min(1, ny + nx));
    S.motors.right = Math.max(-1, Math.min(1, ny - nx));
    D.vml.textContent = Math.round(S.motors.left  * 100) + "%";
    D.vmr.textContent = Math.round(S.motors.right * 100) + "%";
    sendMotors();
  }
})();

// ── Head slider ────────────────────────────────────────────────────────────
D.sHead.addEventListener("input", e => {
  const v = parseInt(e.target.value);
  D.vHead.textContent = v + "%";
  S.motors.head = v / 100;
  sendMotors();
});
D.sHead.addEventListener("pointerup", () => setTimeout(() => {
  D.sHead.value = 0; D.vHead.textContent = "0%";
  S.motors.head = 0; sendMotors();
}, 60));

// ── Lift slider ────────────────────────────────────────────────────────────
D.sLift.addEventListener("input", e => {
  const v = parseInt(e.target.value);
  D.vLift.textContent = v + "%";
  S.motors.lift = v / 100;
  sendMotors();
});
D.sLift.addEventListener("pointerup", () => setTimeout(() => {
  D.sLift.value = 0; D.vLift.textContent = "0%";
  S.motors.lift = 0; sendMotors();
}, 60));

// ── Brightness slider ──────────────────────────────────────────────────────
async function sendBrightness(level) {
  const r = await robotFetch("/display/brightness", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ level }),
  });
  if (!r.ok) throw new Error(await r.text());
}

function queueBrightness(level, delay = 80) {
  if (brightnessTimer) clearTimeout(brightnessTimer);
  brightnessTimer = setTimeout(async () => {
    brightnessTimer = null;
    try {
      await sendBrightness(level);
      log("INFO", `Backlight -> ${level}`);
    } catch (e) {
      log("ERROR", `Backlight failed: ${e.message}`);
    }
  }, delay);
}

D.sBright.addEventListener("input", e => {
  const level = parseInt(e.target.value);
  D.vBright.textContent = level;
  queueBrightness(level);
});
D.sBright.addEventListener("change", e => queueBrightness(parseInt(e.target.value), 0));

// ── LED color picker ───────────────────────────────────────────────────────
D.ledColor.addEventListener("input", e => {
  D.ledPrev.style.background = e.target.value;
  D.ledPrev.style.boxShadow  = `0 0 14px ${e.target.value}88`;
});

document.querySelectorAll(".preset").forEach(btn => {
  btn.addEventListener("click", () => {
    const r = parseInt(btn.dataset.r);
    const g = parseInt(btn.dataset.g);
    const b = parseInt(btn.dataset.b);
    const hex = "#" + [r, g, b].map(x => x.toString(16).padStart(2, "0")).join("");
    D.ledColor.value = hex;
    D.ledPrev.style.background = hex;
    D.ledPrev.style.boxShadow  = (r || g || b) ? `0 0 14px ${hex}88` : "none";
    sendLedsRgb(r, g, b);
  });
});

// ── Face display media upload ──────────────────────────────────────────────
const FACE_W = 184;
const FACE_H = 96;
const AUDIO_SAMPLE_RATE = 16000;
const AUDIO_MAX_BYTES = 4 * 1024 * 1024;
const AUDIO_CHUNK_BYTES = Math.floor(AUDIO_MAX_BYTES * 0.9);
const AUDIO_CHUNK_SAMPLES = Math.floor((AUDIO_CHUNK_BYTES - 44) / 2);
let displayMediaKind = null;
let displayMediaFile = null;
let displayMediaUrl = null;
let videoTimer = null;
let audioPlayToken = 0;
let brightnessTimer = null;
let volumeTimer = null;

function robotFetch(path, opts = {}) {
  const ip = D.ip.value.trim();
  return fetch(`/api${path}`, {
    ...opts,
    headers: { ...(opts.headers || {}), "x-robot-ip": ip },
  });
}

function setDisplayStatus(text, cls = "") {
  D.vDisplay.textContent = text;
  D.vDisplay.className = "val " + cls;
}

function setAudioStatus(text, cls = "") {
  D.vAudio.textContent = text;
  D.vAudio.className = "val " + cls;
}

function rgb565FromPreview() {
  const ctx = D.facePreview.getContext("2d");
  const img = ctx.getImageData(0, 0, FACE_W, FACE_H).data;
  const out = new Uint8Array(FACE_W * FACE_H * 2);
  for (let i = 0, j = 0; i < img.length; i += 4, j += 2) {
    const r = img[i] >> 3;
    const g = img[i + 1] >> 2;
    const b = img[i + 2] >> 3;
    const v = (r << 11) | (g << 5) | b;
    out[j] = v & 0xff;
    out[j + 1] = v >> 8;
  }
  return out;
}

async function sendDisplayFrame() {
  try {
    const r = await robotFetch("/display/frame", {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: rgb565FromPreview(),
    });
    if (!r.ok) throw new Error(await r.text());
    setDisplayStatus("SENT", "ok");
  } catch (e) {
    setDisplayStatus("ERROR", "bad");
    log("ERROR", `Display frame failed: ${e.message}`);
  }
}

function drawImageToFace(img) {
  const ctx = D.facePreview.getContext("2d");
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, FACE_W, FACE_H);
  const scale = Math.min(FACE_W / img.naturalWidth, FACE_H / img.naturalHeight);
  const w = Math.max(1, Math.round(img.naturalWidth * scale));
  const h = Math.max(1, Math.round(img.naturalHeight * scale));
  ctx.drawImage(img, Math.floor((FACE_W - w) / 2), Math.floor((FACE_H - h) / 2), w, h);
}

function drawVideoToFace() {
  const v = D.videoSource;
  if (!v.videoWidth) return;
  const ctx = D.facePreview.getContext("2d");
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, FACE_W, FACE_H);
  const scale = Math.min(FACE_W / v.videoWidth, FACE_H / v.videoHeight);
  const w = Math.max(1, Math.round(v.videoWidth * scale));
  const h = Math.max(1, Math.round(v.videoHeight * scale));
  ctx.drawImage(v, Math.floor((FACE_W - w) / 2), Math.floor((FACE_H - h) / 2), w, h);
}

D.displayFile.addEventListener("change", () => {
  stopVideoStream({ cancelAudio: true });
  releaseDisplayMediaUrl();
  const file = D.displayFile.files?.[0];
  if (!file) return;
  displayMediaFile = file;
  const url = URL.createObjectURL(file);
  displayMediaUrl = url;
  if (file.type.startsWith("video/")) {
    displayMediaKind = "video";
    D.videoSource.src = url;
    D.videoSource.onloadeddata = () => {
      drawVideoToFace();
      setDisplayStatus("VIDEO");
    };
  } else {
    displayMediaKind = "image";
    displayMediaFile = null;
    const img = new Image();
    img.onload = () => {
      drawImageToFace(img);
      setDisplayStatus("IMAGE");
      releaseDisplayMediaUrl();
    };
    img.src = url;
  }
});

D.displayInit.addEventListener("click", async () => {
  try {
    const r = await robotFetch("/display/init", { method: "POST" });
    if (!r.ok) throw new Error(await r.text());
    setDisplayStatus("INIT OK", "ok");
    log("OK", "Display initialized.");
  } catch (e) {
    setDisplayStatus("INIT ERR", "bad");
    log("ERROR", `Display init failed: ${e.message}`);
  }
});

D.displaySend.addEventListener("click", sendDisplayFrame);

function releaseDisplayMediaUrl() {
  if (!displayMediaUrl) return;
  URL.revokeObjectURL(displayMediaUrl);
  displayMediaUrl = null;
}

function stopVideoStream({ cancelAudio = false, releaseUrl = false } = {}) {
  if (videoTimer) clearInterval(videoTimer);
  videoTimer = null;
  D.videoToggle.textContent = "PLAY VIDEO";
  if (!D.videoSource.paused) D.videoSource.pause();
  if (cancelAudio) {
    audioPlayToken++;
    robotFetch("/audio/stop", { method: "POST" }).catch(() => {});
  }
  if (releaseUrl) releaseDisplayMediaUrl();
}

D.videoToggle.addEventListener("click", async () => {
  if (displayMediaKind !== "video") return;
  if (videoTimer) {
    stopVideoStream({ cancelAudio: true });
    setDisplayStatus("VIDEO PAUSED");
    return;
  }

  const token = ++audioPlayToken;
  const audioReady = displayMediaFile
    ? prepareAudioChunks(displayMediaFile, token, displayMediaFile.name)
    : Promise.resolve(null);
  setDisplayStatus("AUDIO PREP");
  const preparedAudio = await audioReady.catch(e => {
    log("WARN", `Video audio unavailable: ${e.message}`);
    return null;
  });
  if (token !== audioPlayToken) return;

  D.videoSource.currentTime = 0;
  await D.videoSource.play();
  D.videoToggle.textContent = "STOP VIDEO";
  videoTimer = setInterval(() => {
    drawVideoToFace();
    sendDisplayFrame();
  }, 120);

  if (preparedAudio) {
    playPreparedAudioChunks(preparedAudio, token).catch(e => {
      if (token !== audioPlayToken) return;
      setAudioStatus("ERROR", "bad");
      log("ERROR", `Video audio failed: ${e.message}`);
    });
  }
});

D.videoSource.addEventListener("ended", () => {
  stopVideoStream({ cancelAudio: true, releaseUrl: true });
  displayMediaKind = null;
  displayMediaFile = null;
  setDisplayStatus("VIDEO DONE", "ok");
});

// ── Audio upload ───────────────────────────────────────────────────────────
function currentVolumeGain() {
  return 1;
}

function encodeWavPcm16(samples, sampleRate, gain = 1) {
  const bytes = 44 + samples.length * 2;
  const out = new ArrayBuffer(bytes);
  const view = new DataView(out);
  const writeAscii = (offset, text) => {
    for (let i = 0; i < text.length; i++) view.setUint8(offset + i, text.charCodeAt(i));
  };

  writeAscii(0, "RIFF");
  view.setUint32(4, bytes - 8, true);
  writeAscii(8, "WAVE");
  writeAscii(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeAscii(36, "data");
  view.setUint32(40, samples.length * 2, true);

  let offset = 44;
  for (let i = 0; i < samples.length; i++, offset += 2) {
    const s = Math.max(-1, Math.min(1, samples[i] * gain));
    view.setInt16(offset, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }
  return new Uint8Array(out);
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function decodeAudioFileToMono(file) {
  const input = await file.arrayBuffer();
  const AudioContextCtor = window.AudioContext || window.webkitAudioContext;
  if (!AudioContextCtor || !window.OfflineAudioContext) {
    throw new Error("browser audio conversion is unavailable");
  }

  const audioCtx = new AudioContextCtor();
  let decoded;
  try {
    decoded = await audioCtx.decodeAudioData(input.slice(0));
  } finally {
    audioCtx.close?.();
  }

  const frameCount = Math.max(1, Math.ceil(decoded.duration * AUDIO_SAMPLE_RATE));
  const offline = new OfflineAudioContext(1, frameCount, AUDIO_SAMPLE_RATE);
  const source = offline.createBufferSource();
  source.buffer = decoded;
  source.connect(offline.destination);
  source.start(0);
  const rendered = await offline.startRendering();
  return rendered.getChannelData(0);
}

async function uploadAudioWavChunk(wav) {
  const r = await robotFetch("/audio/play", {
    method: "POST",
    headers: { "Content-Type": "audio/wav" },
    body: wav,
  });
  if (!r.ok) throw new Error(await r.text());
}

async function sendVolume(level) {
  const r = await robotFetch("/audio/volume", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ level }),
  });
  if (!r.ok) throw new Error(await r.text());
}

function queueVolume(level, delay = 80) {
  if (volumeTimer) clearTimeout(volumeTimer);
  volumeTimer = setTimeout(async () => {
    volumeTimer = null;
    try {
      await sendVolume(level);
      log("INFO", `Volume -> ${level}%`);
    } catch (e) {
      log("ERROR", `Volume failed: ${e.message}`);
    }
  }, delay);
}

async function prepareAudioChunks(file, token, label) {
  setAudioStatus("CONVERTING");
  const samples = await decodeAudioFileToMono(file);
  if (token !== audioPlayToken) return null;
  const chunks = Math.max(1, Math.ceil(samples.length / AUDIO_CHUNK_SAMPLES));
  const totalKiB = Math.round((44 * chunks + samples.length * 2) / 1024);
  log("INFO", `Audio converted: ${label} (${chunks} chunk${chunks === 1 ? "" : "s"}, ${totalKiB} KiB WAV).`);
  return { samples, chunks, label };
}

async function playPreparedAudioChunks(prepared, token) {
  const { samples, chunks, label } = prepared;
  for (let chunk = 0; chunk < chunks; chunk++) {
    if (token !== audioPlayToken) return;
    const start = chunk * AUDIO_CHUNK_SAMPLES;
    const end = Math.min(samples.length, start + AUDIO_CHUNK_SAMPLES);
    const gain = currentVolumeGain();
    const wav = encodeWavPcm16(samples.subarray(start, end), AUDIO_SAMPLE_RATE, gain);
    setAudioStatus(`PLAY ${chunk + 1}/${chunks}`, "ok");
    await uploadAudioWavChunk(wav);

    if (chunk < chunks - 1) {
      const durationMs = ((end - start) / AUDIO_SAMPLE_RATE) * 1000;
      await sleep(durationMs + 80);
    }
  }

  if (token !== audioPlayToken) return;
  setAudioStatus("PLAYING", "ok");
  log("OK", `Audio playback queued: ${label}`);
}

D.audioPlay.addEventListener("click", async () => {
  const file = D.audioFile.files?.[0];
  if (!file) { log("WARN", "Choose an audio file first."); return; }
  const token = ++audioPlayToken;
  try {
    const prepared = await prepareAudioChunks(file, token, file.name);
    if (!prepared) return;
    await playPreparedAudioChunks(prepared, token);
  } catch (e) {
    if (token !== audioPlayToken) return;
    setAudioStatus("ERROR", "bad");
    log("ERROR", `Audio failed: ${e.message}`);
  }
});

D.sVolume.addEventListener("input", e => {
  const level = parseInt(e.target.value);
  D.vVolume.textContent = `${level}%`;
  queueVolume(level);
});

D.sVolume.addEventListener("change", e => queueVolume(parseInt(e.target.value), 0));

D.audioStop.addEventListener("click", async () => {
  audioPlayToken++;
  await robotFetch("/audio/stop", { method: "POST" }).catch(() => {});
  setAudioStatus("STOPPED");
});

// ── Camera panel UI (snapshot-polling mode) ───────────────────────────────────
// We do NOT use MJPEG <img> because the Bun proxy can stall when the robot
// is slow. Instead we poll /api/camera/snapshot every 500ms and display
// as a dataURL — this gives reliable ~2fps even with a cold daemon.
let camStreamActive = false;
let camStreamTimer  = null;
let camImgEl        = null;
let camLastUrl      = null;
let camFrameCount   = 0;
let camFpsClock     = Date.now();

function setCamDaemonStatus(text, cls = "") {
  if (D.vcamDaemon) { D.vcamDaemon.textContent = text; D.vcamDaemon.className = "val " + cls; }
}
function setCamStreamStatus(text, cls = "") {
  if (D.vcamStream) { D.vcamStream.textContent = text; D.vcamStream.className = "val " + cls; }
}

function camEnsureImg() {
  if (!camImgEl) {
    camImgEl = document.createElement("img");
    camImgEl.alt = "Camera";
    camImgEl.style.cssText = "width:100%;height:100%;object-fit:contain;display:block";
    if (D.camFeedWrap) D.camFeedWrap.appendChild(camImgEl);
  }
  if (D.camOffline) D.camOffline.style.display = "none";
}

async function camPollFrame() {
  if (!camStreamActive) return;
  try {
    const resp = await robotFetch("/camera/snapshot", {}, { noThrow: true });
    if (resp && resp.ok) {
      const blob = await resp.blob();
      const url  = URL.createObjectURL(blob);
      camEnsureImg();
      camImgEl.src = url;
      // Revoke previous URL to free memory
      if (camLastUrl) setTimeout(() => URL.revokeObjectURL(camLastUrl), 2000);
      camLastUrl = url;
      // FPS counter
      camFrameCount++;
      const now = Date.now();
      if (now - camFpsClock >= 2000) {
        const fps = (camFrameCount / ((now - camFpsClock) / 1000)).toFixed(1);
        if (D.vcamFps) D.vcamFps.textContent = fps;
        camFrameCount = 0;
        camFpsClock = now;
      }
      setCamStreamStatus("LIVE", "ok");
    } else {
      setCamStreamStatus("NO FRAME", "bad");
    }
  } catch (e) {
    setCamStreamStatus("ERROR", "bad");
  }
  if (camStreamActive) camStreamTimer = setTimeout(camPollFrame, 500);
}

function startCamStream() {
  if (camStreamActive) return;
  camStreamActive = true;
  camFrameCount = 0; camFpsClock = Date.now();
  setCamStreamStatus("CONNECTING", "");
  if (D.vcamFps) D.vcamFps.textContent = "--";
  if (D.btnCamStreamOn) D.btnCamStreamOn.textContent = "⏹ STOP STREAM";
  log("INFO", "Camera stream started (snapshot polling 2fps).");
  camPollFrame();
}

function stopCamStream() {
  camStreamActive = false;
  if (camStreamTimer) { clearTimeout(camStreamTimer); camStreamTimer = null; }
  if (camImgEl && D.camFeedWrap && D.camFeedWrap.contains(camImgEl)) {
    D.camFeedWrap.removeChild(camImgEl);
    camImgEl = null;
  }
  if (camLastUrl) { URL.revokeObjectURL(camLastUrl); camLastUrl = null; }
  if (D.camOffline) D.camOffline.style.display = "";
  setCamStreamStatus("IDLE", "");
  if (D.vcamFps) D.vcamFps.textContent = "--";
  if (D.btnCamStreamOn) D.btnCamStreamOn.textContent = "► START STREAM";
}

if (D.btnCamStart) {
  D.btnCamStart.addEventListener("click", async () => {
    try {
      const r = await robotFetch("/camera/daemon/start", { method: "POST" });
      const j = await r.json();
      if (r.ok && j.ok) {
        setCamDaemonStatus(`RUNNING (pid ${j.pid})`, "ok");
        log("OK", `Camera daemon started, pid=${j.pid}`);
      } else {
        setCamDaemonStatus("FAILED", "bad");
        log("ERROR", `Camera daemon: ${j.error || "unknown error"}`);
      }
    } catch (e) {
      setCamDaemonStatus("ERROR", "bad");
      log("ERROR", `Camera daemon start: ${e.message}`);
    }
  });
}

if (D.btnCamStop) {
  D.btnCamStop.addEventListener("click", async () => {
    stopCamStream();
    try {
      await robotFetch("/camera/daemon/stop", { method: "POST" });
      setCamDaemonStatus("STOPPED", "");
      log("INFO", "Camera daemon stopped.");
    } catch (e) {
      log("WARN", `Camera daemon stop: ${e.message}`);
    }
  });
}

if (D.btnCamStreamOn) {
  D.btnCamStreamOn.addEventListener("click", () => {
    if (camStreamActive) stopCamStream();
    else startCamStream();
  });
}

if (D.btnCamSnapshot) {
  D.btnCamSnapshot.addEventListener("click", async () => {
    try {
      const r = await robotFetch("/camera/snapshot");
      if (!r.ok) throw new Error(await r.text());
      const blob = await r.blob();
      const url  = URL.createObjectURL(blob);
      camEnsureImg();
      camImgEl.src = url;
      setTimeout(() => URL.revokeObjectURL(url), 10000);
      const a = document.createElement("a");
      a.href = url; a.download = `vector-${Date.now()}.bmp`;
      document.body.appendChild(a); a.click(); document.body.removeChild(a);
      log("OK", "Snapshot saved.");
      setCamStreamStatus("SNAPSHOT", "ok");
    } catch (e) {
      log("ERROR", `Snapshot: ${e.message}`);
    }
  });
}

// ── Drive direction canvas ─────────────────────────────────────────────────
// Shows a live arrow of the robot's drive vector (direction + speed).
// Much more useful than the broken proximity radar.
(function driveVis() {
  const canvas = D.driveCanvas;
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const W = canvas.width, H = canvas.height;
  const cx = W / 2, cy = H / 2;
  const R  = Math.min(cx, cy) - 10;

  // Smooth display values (separate from server ramp — purely for animation)
  let dispL = 0, dispR = 0;

  function draw() {
    dispL += (S.motors.left  - dispL) * 0.18;
    dispR += (S.motors.right - dispR) * 0.18;

    ctx.clearRect(0, 0, W, H);

    // Outer ring
    ctx.strokeStyle = "rgba(0,240,255,0.12)";
    ctx.lineWidth = 1.5;
    ctx.beginPath(); ctx.arc(cx, cy, R, 0, Math.PI * 2); ctx.stroke();

    // Speed rings
    [0.33, 0.66, 1].forEach(f => {
      ctx.strokeStyle = `rgba(0,240,255,${0.04 + f * 0.04})`;
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.arc(cx, cy, R * f, 0, Math.PI * 2); ctx.stroke();
    });

    // Compute drive vector from tank-drive: fwd = avg(L,R), turn = L-R
    const fwd  = (dispL + dispR) / 2;
    const turn = (dispL - dispR) / 2;
    const speed = Math.hypot(fwd, turn);

    if (speed > 0.02) {
      // Arrow direction: fwd=up(negative Y), turn=right(positive X)
      const angle = Math.atan2(-turn, fwd) - Math.PI / 2;
      const len   = speed * R * 0.85;
      const ax = cx + Math.sin(angle) * len;
      const ay = cy - Math.cos(angle) * len;

      // Glow trail
      const grad = ctx.createLinearGradient(cx, cy, ax, ay);
      grad.addColorStop(0, "rgba(0,240,255,0)");
      grad.addColorStop(1, fwd < 0 ? "rgba(255,46,76,0.9)" : "rgba(0,240,255,0.9)");
      ctx.strokeStyle = grad;
      ctx.lineWidth = 3;
      ctx.lineCap = "round";
      ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(ax, ay); ctx.stroke();

      // Arrowhead
      const hw = 8, hl = 14;
      const perp = angle + Math.PI / 2;
      ctx.fillStyle = fwd < 0 ? "rgba(255,46,76,0.9)" : "rgba(0,240,255,0.9)";
      ctx.beginPath();
      ctx.moveTo(ax, ay);
      ctx.lineTo(ax - Math.sin(angle) * hl + Math.cos(perp) * hw,
                 ay + Math.cos(angle) * hl + Math.sin(perp) * hw);
      ctx.lineTo(ax - Math.sin(angle) * hl - Math.cos(perp) * hw,
                 ay + Math.cos(angle) * hl - Math.sin(perp) * hw);
      ctx.closePath(); ctx.fill();

      // Speed label
      ctx.fillStyle = "rgba(0,240,255,0.6)";
      ctx.font = "bold 11px 'JetBrains Mono', monospace";
      ctx.textAlign = "center";
      ctx.fillText(`${Math.round(speed * 100)}%`, cx, H - 10);
    } else {
      ctx.fillStyle = "rgba(0,240,255,0.2)";
      ctx.beginPath(); ctx.arc(cx, cy, 5, 0, Math.PI * 2); ctx.fill();
      ctx.fillStyle = "rgba(0,240,255,0.25)";
      ctx.font = "10px 'JetBrains Mono', monospace";
      ctx.textAlign = "center";
      ctx.fillText("STOPPED", cx, H - 10);
    }

    requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);
})();

// ── Microphone Live Streaming and Beamforming Recording ────────────────────
let micStreamActive = false;
let micAudioCtx = null;
let micNextPlayTime = 0;
const MIC_SAMPLE_RATE = 16000;
let micRecordingActive = false;
let recordedSamples = [];

function playMicChunk(floatSamples) {
  if (!micAudioCtx) return;
  const buffer = micMicContextBuffer(floatSamples);
  if (!buffer) return;
  
  const source = micAudioCtx.createBufferSource();
  source.buffer = buffer;
  source.connect(micAudioCtx.destination);
  
  const currentTime = micAudioCtx.currentTime;
  if (micNextPlayTime < currentTime) {
    micNextPlayTime = currentTime + 0.04;
  }
  source.start(micNextPlayTime);
  micNextPlayTime += buffer.duration;
}

function micMicContextBuffer(samples) {
  if (!micAudioCtx) return null;
  const buffer = micAudioCtx.createBuffer(1, samples.length, MIC_SAMPLE_RATE);
  buffer.copyToChannel(samples, 0);
  return buffer;
}

function toggleMicStream() {
  if (micStreamActive) {
    micStreamActive = false;
    D.btnMicToggle.textContent = "START LISTENING";
    D.btnMicToggle.className = "btn primary";
    D.vmicStatus.textContent = "OFFLINE";
    D.vmicStatus.className = "val";
    
    if (micRecordingActive) {
      toggleMicRecord();
    }
    D.btnMicRecord.disabled = true;
    
    if (S.ws && S.ws.readyState === WebSocket.OPEN) {
      S.ws.send(JSON.stringify({ type: "mic_stop" }));
    }
    if (micAudioCtx) {
      try { micAudioCtx.close(); } catch(_) {}
      micAudioCtx = null;
    }
    log("INFO", "Microphone stream stopped.");
  } else {
    micStreamActive = true;
    D.btnMicToggle.textContent = "STOP LISTENING";
    D.btnMicToggle.className = "btn danger";
    D.vmicStatus.textContent = "LISTENING";
    D.vmicStatus.className = "val ok";
    D.btnMicRecord.disabled = false;
    
    const AudioContextCtor = window.AudioContext || window.webkitAudioContext;
    micAudioCtx = new AudioContextCtor();
    micNextPlayTime = micAudioCtx.currentTime;
    
    if (S.ws && S.ws.readyState === WebSocket.OPEN) {
      S.ws.send(JSON.stringify({ type: "mic_start" }));
    }
    log("INFO", "Microphone stream requested.");
  }
}

function toggleMicRecord() {
  if (micRecordingActive) {
    micRecordingActive = false;
    D.btnMicRecord.textContent = "RECORD";
    D.btnMicRecord.className = "btn danger";
    
    if (recordedSamples.length > 0) {
      try {
        const wav = encodeWavPcm16(recordedSamples, MIC_SAMPLE_RATE);
        const blob = new Blob([wav], { type: "audio/wav" });
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = `vector-mic-stream-${Date.now()}.wav`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        log("OK", `Mic recording saved: ${recordedSamples.length} samples.`);
      } catch (e) {
        log("ERROR", `Failed to save recording: ${e.message}`);
      }
    }
    recordedSamples = [];
  } else {
    micRecordingActive = true;
    recordedSamples = [];
    D.btnMicRecord.textContent = "STOP & SAVE";
    D.btnMicRecord.className = "btn primary animate-pulse";
    log("INFO", "Recording microphone stream...");
  }
}

// ── Tab switching ─────────────────────────────────────────────────────────────────
function initTabs() {
  const btns  = document.querySelectorAll(".tab-btn");
  const panes = document.querySelectorAll(".tab-pane");
  btns.forEach(btn => {
    btn.addEventListener("click", () => {
      btns.forEach(b  => b.classList.remove("active"));
      panes.forEach(p => p.classList.remove("active"));
      btn.classList.add("active");
      const pane = document.getElementById(btn.dataset.tab);
      if (pane) pane.classList.add("active");
    });
  });
}

// ── Init ───────────────────────────────────────────────────────────────────
window.addEventListener("load", () => {
  const saved = localStorage.getItem("vec-ip");
  if (saved) D.ip.value = saved;
  const faceCtx = D.facePreview.getContext("2d");
  faceCtx.fillStyle = "#000";
  faceCtx.fillRect(0, 0, FACE_W, FACE_H);
  D.ledPrev.style.background = D.ledColor.value;
  D.ledPrev.style.boxShadow  = `0 0 14px ${D.ledColor.value}88`;

  initTabs();

  if (D.btnMicToggle) D.btnMicToggle.addEventListener("click", toggleMicStream);
  if (D.btnMicRecord) D.btnMicRecord.addEventListener("click", toggleMicRecord);

  // Position control buttons
  if (D.btnGotoLift)   D.btnGotoLift.addEventListener("click",  () => gotoMotor(2, D.gotoLiftTicks));
  if (D.btnGotoHead)   D.btnGotoHead.addEventListener("click",  () => gotoMotor(3, D.gotoHeadTicks));
  if (D.btnHoldLift)   D.btnHoldLift.addEventListener("click",  () => toggleHold(2, D.btnHoldLift, "Lift"));
  if (D.btnHoldHead)   D.btnHoldHead.addEventListener("click",  () => toggleHold(3, D.btnHoldHead, "Head"));
  if (D.btnMotorsStop) D.btnMotorsStop.addEventListener("click", () => {
    stopMotors().catch(e => log("WARN", `Motor stop failed: ${e.message}`));
    S.motors.left = S.motors.right = S.motors.lift = S.motors.head = 0;
    holdState.lift = holdState.head = false;
    holdTargetPos.lift = holdTargetPos.head = null;
    for (const btn of [D.btnHoldLift, D.btnHoldHead]) {
      if (!btn) continue;
      btn.textContent = "HOLD";
      btn.classList.remove("active");
    }
    sendMotors();
    log("INFO", "All motors stopped.");
  });

  log("INFO", "Ready. Click CONNECT.");
  log("WARN", "Touch sensor 1: not populated on this unit — disabled.");
});
