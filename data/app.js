const $ = id => document.getElementById(id);

const modeEl       = $("mode");
const colorEl      = $("color");
const paletteEl    = $("palette");
const powerOnEl    = $("powerOn");
const periodEl     = $("period");
const minBrightEl  = $("minBright");
const maxBrightEl  = $("maxBright");
const brightnessEl = $("brightness");
const applyBtnEl   = $("applyBtn");
const statusEl     = $("status");
const colorCardEl  = $("colorCard");
const breathCardEl = $("breathCard");
const customPalEl  = $("customPalette");
const cpInputs     = Array.from(document.querySelectorAll(".cp"));

// ── label helpers ──────────────────────────────────────────────────────────
function pct(v) { return Math.round(v / 255 * 100) + "%"; }

periodEl.addEventListener("input",     () => { $("periodVal").textContent  = (periodEl.value / 1000).toFixed(1) + "s"; });
minBrightEl.addEventListener("input",  () => { $("minVal").textContent     = pct(minBrightEl.value); });
maxBrightEl.addEventListener("input",  () => { $("maxVal").textContent     = pct(maxBrightEl.value); });
brightnessEl.addEventListener("input", () => { $("brightVal").textContent  = pct(brightnessEl.value); });

// ── visibility ─────────────────────────────────────────────────────────────
function refreshVisibility() {
  const breathing = modeEl.value === "palette_breathing";
  colorCardEl.style.display  = breathing ? "none"  : "block";
  breathCardEl.style.display = breathing ? "block" : "none";
  customPalEl.style.display  = (breathing && paletteEl.value === "custom") ? "block" : "none";
}
modeEl.addEventListener("change", refreshVisibility);
paletteEl.addEventListener("change", refreshVisibility);

// ── load state from ESP ────────────────────────────────────────────────────
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

  if (Array.isArray(state.custom_palette)) {
    state.custom_palette.forEach((hex, i) => {
      if (cpInputs[i]) cpInputs[i].value = hex;
    });
  }

  refreshVisibility();
}

// ── apply state to ESP ─────────────────────────────────────────────────────
async function applyState() {
  const payload = {
    mode:           modeEl.value,
    power_on:       powerOnEl.checked,
    color:          colorEl.value,
    palette:        paletteEl.value,
    brightness_cap: parseInt(brightnessEl.value, 10),
    period_ms:      parseInt(periodEl.value, 10),
    min_val:        parseInt(minBrightEl.value, 10),
    max_val:        parseInt(maxBrightEl.value, 10),
    custom_palette: cpInputs.map(el => el.value),
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
