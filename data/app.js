const modeEl = document.getElementById("mode");
const colorEl = document.getElementById("color");
const paletteEl = document.getElementById("palette");
const powerOnEl = document.getElementById("powerOn");
const colorRowEl = document.getElementById("colorRow");
const paletteRowEl = document.getElementById("paletteRow");
const statusEl = document.getElementById("status");
const applyBtnEl = document.getElementById("applyBtn");

function refreshVisibility() {
  const isSingle = modeEl.value === "single_color";
  colorRowEl.style.display = isSingle ? "flex" : "none";
  paletteRowEl.style.display = isSingle ? "none" : "flex";
}

async function loadState() {
  const res = await fetch("/api/state", { cache: "no-store" });
  const state = await res.json();
  modeEl.value = state.mode || "single_color";
  colorEl.value = state.color || "#2A7BFF";
  paletteEl.value = state.palette || "sunset";
  powerOnEl.checked = !!state.power_on;
  refreshVisibility();
}

async function applyState() {
  const payload = {
    mode: modeEl.value,
    power_on: powerOnEl.checked,
    color: colorEl.value,
    palette: paletteEl.value
  };
  statusEl.textContent = "Applying...";
  await fetch("/api/control", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload)
  });
  statusEl.textContent = "Updated";
}

modeEl.addEventListener("change", refreshVisibility);
applyBtnEl.addEventListener("click", () => applyState().catch(() => {
  statusEl.textContent = "Update failed";
}));
loadState().catch(() => {
  statusEl.textContent = "Failed to load state";
});
