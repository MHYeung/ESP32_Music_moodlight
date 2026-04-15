const $ = id => document.getElementById(id);

const modeEl        = $("mode");
const colorEl       = $("color");
const paletteEl     = $("palette");
const powerOnEl     = $("powerOn");
const periodEl      = $("period");
const minBrightEl   = $("minBright");
const maxBrightEl   = $("maxBright");
const brightnessEl  = $("brightness");
const bpmEl         = $("bpm");
const beatPctEl     = $("beatPct");
const sensitivityEl = $("sensitivity");
const noiseFloorEl  = $("noiseFloor");
const emaAlphaEl    = $("emaAlpha");
const hueSpreadEl   = $("hueSpread");
const applyBtnEl    = $("applyBtn");
const statusEl      = $("status");

const colorCardEl  = $("colorCard");
const breathCardEl = $("breathCard");
const beatCardEl   = $("beatCard");
const musicCardEl  = $("musicCard");
const customPalEl  = $("customPalette");
const cpInputs     = Array.from(document.querySelectorAll(".cp"));

// ── label helpers ───────────────────────────────────────────────────────────
function pct(v) { return Math.round(v / 255 * 100) + "%"; }

periodEl.addEventListener    ("input", () => { $("periodVal").textContent  = (periodEl.value / 1000).toFixed(1) + "s"; });
minBrightEl.addEventListener ("input", () => { $("minVal").textContent     = pct(minBrightEl.value); });
maxBrightEl.addEventListener ("input", () => { $("maxVal").textContent     = pct(maxBrightEl.value); });
brightnessEl.addEventListener("input", () => { $("brightVal").textContent  = pct(brightnessEl.value); });
bpmEl.addEventListener       ("input", () => { $("bpmVal").textContent     = bpmEl.value; });
beatPctEl.addEventListener   ("input", () => { $("beatPctVal").textContent = beatPctEl.value + "%"; });
sensitivityEl.addEventListener("input", () => { $("sensVal").textContent  = sensitivityEl.value; });
noiseFloorEl.addEventListener ("input", () => { $("floorVal").textContent  = noiseFloorEl.value; });
emaAlphaEl.addEventListener   ("input", () => { $("alphaVal").textContent  = (emaAlphaEl.value / 100).toFixed(2); });
hueSpreadEl.addEventListener  ("input", () => { $("spreadVal").textContent = hueSpreadEl.value + "°"; });

// ── card visibility ─────────────────────────────────────────────────────────
function refreshVisibility() {
  const mode = modeEl.value;
  // Color picker is shared by Single Color and Beat Flash (flash uses manual color)
  colorCardEl.style.display  = (mode === "single_color" || mode === "beat_flash") ? "block" : "none";
  breathCardEl.style.display = mode === "palette_breathing" ? "block" : "none";
  beatCardEl.style.display   = mode === "beat_flash"        ? "block" : "none";
  musicCardEl.style.display  = mode === "music_react"       ? "block" : "none";
  customPalEl.style.display  = (mode === "palette_breathing" && paletteEl.value === "custom") ? "block" : "none";
}
modeEl.addEventListener   ("change", refreshVisibility);
paletteEl.addEventListener("change", refreshVisibility);

// ── load state from ESP ─────────────────────────────────────────────────────
async function loadState() {
  const res   = await fetch("/api/state", { cache: "no-store" });
  const state = await res.json();

  modeEl.value      = state.mode      || "single_color";
  colorEl.value     = state.color     || "#2A7BFF";
  paletteEl.value   = state.palette   || "sunset";
  powerOnEl.checked = !!state.power_on;

  brightnessEl.value = state.brightness_cap ?? 128;
  $("brightVal").textContent = pct(brightnessEl.value);

  periodEl.value = state.period_ms ?? 2600;
  $("periodVal").textContent = (periodEl.value / 1000).toFixed(1) + "s";

  minBrightEl.value = state.min_val ?? 16;
  $("minVal").textContent = pct(minBrightEl.value);

  maxBrightEl.value = state.max_val ?? 160;
  $("maxVal").textContent = pct(maxBrightEl.value);

  bpmEl.value = state.bpm ?? 120;
  $("bpmVal").textContent = bpmEl.value;

  beatPctEl.value = state.beat_on_pct ?? 20;
  $("beatPctVal").textContent = beatPctEl.value + "%";

  sensitivityEl.value = state.music_sensitivity ?? 5;
  $("sensVal").textContent = sensitivityEl.value;

  noiseFloorEl.value = state.music_noise_floor ?? 10;
  $("floorVal").textContent = noiseFloorEl.value;

  emaAlphaEl.value = state.ema_alpha ?? 20;
  $("alphaVal").textContent = (emaAlphaEl.value / 100).toFixed(2);

  hueSpreadEl.value = state.hue_spread ?? 60;
  $("spreadVal").textContent = hueSpreadEl.value + "°";

  if (Array.isArray(state.custom_palette)) {
    state.custom_palette.forEach((hex, i) => {
      if (cpInputs[i]) cpInputs[i].value = hex;
    });
  }

  refreshVisibility();
}

// ── apply state to ESP ──────────────────────────────────────────────────────
async function applyState() {
  const payload = {
    mode:                modeEl.value,
    power_on:            powerOnEl.checked,
    color:               colorEl.value,
    palette:             paletteEl.value,
    brightness_cap:      parseInt(brightnessEl.value, 10),
    period_ms:           parseInt(periodEl.value, 10),
    min_val:             parseInt(minBrightEl.value, 10),
    max_val:             parseInt(maxBrightEl.value, 10),
    bpm:                 parseInt(bpmEl.value, 10),
    beat_on_pct:         parseInt(beatPctEl.value, 10),
    music_sensitivity:   parseInt(sensitivityEl.value, 10),
    music_noise_floor:   parseInt(noiseFloorEl.value, 10),
    ema_alpha:           parseInt(emaAlphaEl.value, 10),
    hue_spread:          parseInt(hueSpreadEl.value, 10),
    custom_palette:      cpInputs.map(el => el.value),
  };

  statusEl.textContent = "Applying…";
  statusEl.className   = "status";

  const res = await fetch("/api/control", {
    method:  "POST",
    headers: { "Content-Type": "application/json" },
    body:    JSON.stringify(payload),
  });
  if (res.ok) {
    statusEl.textContent = "Updated ✓";
    statusEl.className   = "status ok";
  } else {
    statusEl.textContent = "Update failed";
    statusEl.className   = "status err";
  }
}

applyBtnEl.addEventListener("click", () =>
  applyState().catch(() => {
    statusEl.textContent = "Connection error";
    statusEl.className   = "status err";
  })
);

loadState().catch(() => {
  statusEl.textContent = "Failed to load state";
  statusEl.className   = "status err";
});
