import { MIN_MHZ, MAX_MHZ } from "./c5mk-link.js";
import { sweep } from "./vtxscan.js";

export function spectrumFrequencies(lo = MIN_MHZ, hi = MAX_MHZ, step = 5) {
  if (!Number.isInteger(lo) || !Number.isInteger(hi) || lo < MIN_MHZ || hi > MAX_MHZ || hi <= lo || ![2, 5, 10].includes(step)) throw new Error("Invalid spectrum range");
  const frequencies = [];
  for (let mhz = lo; mhz <= hi; mhz += step) frequencies.push(mhz);
  if (frequencies.at(-1) !== hi) frequencies.push(hi);
  return frequencies;
}

export class SpectrumHistory {
  constructor() { this.levels = new Map(); this.peaks = new Map(); this.rows = []; }
  update(levels) {
    for (const [mhz, db] of levels) {
      // A missing measurement stays a gap; it must not become a zero-power bin.
      this.levels.set(mhz, db);
      if (Number.isFinite(db)) this.peaks.set(mhz, Math.max(db, this.peaks.get(mhz) ?? -Infinity));
    }
  }
  complete(at = Date.now()) {
    this.rows.unshift({ at, levels: new Map(this.levels) });
    this.rows.length = Math.min(60, this.rows.length);
  }
}

const CSS = `
.sp-controls { display:flex; flex-wrap:wrap; align-items:center; gap:10px; margin-bottom:10px; }
.sp-controls label { display:flex; gap:6px; align-items:center; color:var(--soft); min-width:0; max-width:100%; min-height:44px; }
.sp-controls select { min-width:0; max-width:100%; flex:1; }
.sp-controls button, .sp-controls select { min-height:48px; font-size:16px; }
.sp-controls input { width:22px; height:22px; }
.sp-plot { height:190px; touch-action:pan-y; }
.sp-status { min-height:24px; margin:8px 0; color:var(--soft); }
.sp-readout { min-height:24px; font:14px var(--mono); color:var(--text); margin:8px 0; }
.sp-note { color:var(--muted); font-size:13px; margin:8px 0; }
.sp-legend { display:flex; flex-wrap:wrap; gap:12px; font-size:13px; color:var(--soft); }
.sp-legend .sp-live { color:#51c9ff; } .sp-legend .sp-peak { color:#e7af50; }
@media (max-width:640px) {
  .sp-plot { height:160px; }
  .sp-controls { gap:8px; }
  .sp-controls button { font-size:14px; }
  .sp-status { font-size:13px; margin:6px 0; }
  .sp-legend { font-size:12px; gap:8px; }
  .sp-note { margin:6px 0; }
}
`;

export function mountSpectrum(container, link, shared = {}) {
  if (!shared.waterfall) throw new Error("Spectrum needs the dashboard's shared waterfall canvas");
  if (!document.getElementById("spectrum-style")) {
    const style = document.createElement("style");
    style.id = "spectrum-style"; style.textContent = CSS; document.head.appendChild(style);
  }
  container.innerHTML = `<div class="sp-controls">
    <button type="button" class="primary" data-sp="start">Start sweep</button>
    <label>View <select data-sp="range"><option value="live" selected>Live pilot channels</option><option value="full">Sweep · 5180–5917 MHz</option><option value="fpv">Sweep · 5645–5917 MHz</option></select></label>
    <label>Step <select data-sp="step"><option value="10">10 MHz · faster</option><option value="5" selected>5 MHz</option><option value="2">2 MHz · finer</option></select></label>
    <label><input type="checkbox" data-sp="hold" checked>Peak hold</label>
    <button type="button" data-sp="clear">Clear peaks</button>
  </div>
  <p class="sp-note" data-sp="note">Live pilot signals · receiver dB</p>
  <div class="sp-legend"><span class="sp-live">━ Latest signal</span><span class="sp-peak">┄ Peak hold</span></div>
  <p class="sp-status" data-sp="status" role="status">Waiting for live pilot readings…</p>
  <canvas class="sp-plot" data-sp="trace" role="img" aria-label="Signal strength by frequency"></canvas>
  <p class="sp-readout" data-sp="readout" aria-live="polite">Waiting for readings</p>
  `;
  const el = key => key === "waterfall" ? shared.waterfall : key === "waterfallNote" ? shared.waterfallNote : container.querySelector(`[data-sp="${key}"]`);
  let history = new SpectrumHistory(), controller = null, selected = null, freqs = [];
  let unavailable = false, liveHistory = new SpectrumHistory(), liveFreqs = [], lastLiveDraw = -Infinity;
  const isLive = () => el("range").value === "live";
  const range = () => isLive() && freqs.length
    ? [Math.max(MIN_MHZ, freqs[0] - 15), Math.min(MAX_MHZ, freqs.at(-1) + 15)]
    : [el("range").value === "fpv" ? 5645 : MIN_MHZ, MAX_MHZ];

  function fit(canvas) {
    const width = canvas.clientWidth, height = canvas.clientHeight;
    if (!width || !height) return null;
    const dpr = globalThis.devicePixelRatio || 1;
    if (canvas.width !== Math.round(width * dpr)) canvas.width = Math.round(width * dpr);
    if (canvas.height !== Math.round(height * dpr)) canvas.height = Math.round(height * dpr);
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, width, height);
    return { ctx, width, height, left: 38, right: width - 12, top: 14, bottom: height - 28 };
  }

  function render() {
    const trace = fit(el("trace")), waterfall = isLive() ? null : fit(el("waterfall"));
    if (!trace || (!isLive() && !waterfall)) return;
    const [lo, hi] = range();
    const values = [...history.levels.values(), ...history.peaks.values()].filter(Number.isFinite);
    const low = Math.floor(Math.min(-5, ...values.map(v => v - 3)) / 10) * 10;
    const high = Math.ceil(Math.max(45, ...values.map(v => v + 3)) / 10) * 10;
    function axes(box, isWaterfall) {
      const {ctx, left, right, top, bottom, width} = box;
      ctx.font = '11px "IBM Plex Sans", sans-serif'; ctx.fillStyle = "#8a919c"; ctx.strokeStyle = "#2a2f37"; ctx.lineWidth = 1;
      const ticks = width < 500 ? 3 : 7;
      for (let i = 0; i <= ticks; i++) {
        const f = lo + (hi - lo) * i / ticks, x = left + (right - left) * i / ticks;
        ctx.textAlign = i === 0 ? "left" : i === ticks ? "right" : "center";
        ctx.fillText(`${Math.round(f)}`, x, bottom + 17);
        if (!isWaterfall) { ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, bottom); ctx.stroke(); }
      }
      ctx.textAlign = "left";
      if (!isWaterfall) {
        for (let i = 0; i <= 5; i++) {
          const y = bottom - (bottom - top) * i / 5;
          ctx.fillText(String(Math.round(low + (high - low) * i / 5)), 0, y + 4);
          ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
        }
        ctx.fillText("dB", 0, 10);
      }
      ctx.textAlign = "right"; ctx.fillText("MHz", right, box.height - 1); ctx.textAlign = "left";
    }
    axes(trace, false);
    const {ctx, left, right, top, bottom} = trace;
    const X = mhz => left + (mhz - lo) / (hi - lo) * (right - left);
    const Y = db => bottom - (db - low) / (high - low) * (bottom - top);
    function line(levels, color, dashed) {
      ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.setLineDash(dashed ? [5, 4] : []);
      ctx.beginPath(); let connected = false;
      for (const mhz of freqs) {
        const db = levels.get(mhz);
        if (!Number.isFinite(db)) { connected = false; continue; }
        if (connected) ctx.lineTo(X(mhz), Y(db)); else ctx.moveTo(X(mhz), Y(db));
        connected = true;
      }
      ctx.stroke(); ctx.setLineDash([]);
    }
    if (el("hold").checked) line(history.peaks, "#e7af50", true);
    line(history.levels, "#51c9ff", false);
    if (isLive()) {
      ctx.fillStyle = "#51c9ff";
      for (const mhz of freqs) {
        const db = history.levels.get(mhz);
        if (Number.isFinite(db)) { ctx.beginPath(); ctx.arc(X(mhz), Y(db), 3, 0, Math.PI * 2); ctx.fill(); }
      }
    }
    if (!values.length) { ctx.fillStyle = "#8a919c"; ctx.fillText(isLive() ? "Waiting for live pilot signals" : "Start a sweep to see the spectrum", left + 8, top + 30); }
    if (waterfall) {
    const wf = waterfall.ctx, rowHeight = (waterfall.bottom - waterfall.top) / 60;
    history.rows.forEach((row, i) => {
      for (let j = 0; j < freqs.length; j++) {
        const db = row.levels.get(freqs[j]);
        if (!Number.isFinite(db)) continue;
        const a = isLive() ? Math.max(lo, freqs[j] - 2) : j ? (freqs[j - 1] + freqs[j]) / 2 : lo;
        const b = isLive() ? Math.min(hi, freqs[j] + 2) : j + 1 < freqs.length ? (freqs[j] + freqs[j + 1]) / 2 : hi;
        const power = Math.max(0, Math.min(1, (db - low) / (high - low)));
        wf.fillStyle = `hsl(${240 - power * 200} 90% ${8 + power * 62}%)`;
        const x = waterfall.left + (a - lo) / (hi - lo) * (waterfall.right - waterfall.left);
        const w = (b - a) / (hi - lo) * (waterfall.right - waterfall.left);
        wf.fillRect(x, waterfall.top + i * rowHeight, w + 0.5, rowHeight + 0.5);
      }
    });
    axes(waterfall, true);
    }
    if (selected !== null) {
      ctx.strokeStyle = "#eef0f3";
      ctx.beginPath(); ctx.moveTo(X(selected), top); ctx.lineTo(X(selected), bottom); ctx.stroke();
      const db = history.levels.get(selected), peak = history.peaks.get(selected);
      el("readout").textContent = `${selected} MHz · ${Number.isFinite(db) ? db.toFixed(1) + " dB" : "no reading"}${Number.isFinite(peak) ? " · peak " + peak.toFixed(1) + " dB" : ""}`;
    } else {
      const best = [...history.levels].filter(([,db]) => Number.isFinite(db)).sort((a,b) => b[1] - a[1])[0];
      el("readout").textContent = best ? `Strongest: ${best[0]} MHz · ${best[1].toFixed(1)} dB` : isLive() ? "Waiting for readings" : "No sweep yet";
    }
  }

  async function start() {
    if (isLive()) return;
    if (controller) { controller.abort(); el("start").textContent = "Stopping…"; return; }
    if (!link.connected) { el("status").textContent = "Connect the receiver first."; return; }
    if (link.beginCheck?.() === false) {
      el("status").textContent = "Wait for VTX check to finish and enable scanning. Spectrum is available when the C5 scans its own channels.";
      return;
    }
    controller = new AbortController(); unavailable = false;
    const signal = controller.signal;
    el("start").textContent = "Stop sweep";
    el("range").disabled = el("step").disabled = true;
    freqs = spectrumFrequencies(...range(), +el("step").value);
    let sweeps = 0, failed = false;
    try {
      while (!signal.aborted) {
        const began = performance.now();
        const levels = await sweep(link, freqs, { signal, onProgress: (done, total, partial) => {
          history.update(partial); render();
          el("status").textContent = `Sweep ${sweeps + 1} · ${Math.round(100 * done / total)}% · pilot history paused`;
        } });
        if (signal.aborted) break;
        history.update(levels);
        if (![...levels.values()].some(Number.isFinite)) throw new Error("No receiver readings. Check the connection and scanning mode.");
        history.complete(); sweeps++;
        render();
        el("status").textContent = `Sweep ${sweeps} complete · ${((performance.now() - began) / 1000).toFixed(1)} s`;
      }
    } catch (error) {
      failed = true; el("status").textContent = error.message;
    } finally {
      try { await link.endCheck?.(); }
      catch (error) { failed = true; el("status").textContent = `Couldn't restore channels: ${error.message}`; }
      controller = null;
      el("start").textContent = "Start sweep";
      el("range").disabled = el("step").disabled = false;
      if (!failed) el("status").textContent = unavailable ? "Sweep stopped: receiver disconnected or FPVGate took control." : "Sweep stopped · original channels restored.";
    }
  }
  el("start").onclick = start;
  el("clear").onclick = () => { history.peaks.clear(); render(); };
  el("hold").onchange = render;
  function selectView() {
    history = isLive() ? liveHistory : new SpectrumHistory();
    freqs = isLive() ? liveFreqs : spectrumFrequencies(...range(), +el("step").value);
    selected = null;
    el("start").hidden = isLive();
    el("step").parentElement.hidden = isLive();
    el("note").textContent = isLive() ? "Updates with your pilot channels · receiver dB" : "Pilot history pauses during sweeps. Stop to resume your original channels.";
    el("note").hidden = isLive();
    el("waterfallNote").textContent = isLive() ? "Newest at top · highest signal per slice · tap a channel to inspect." : "Newest complete sweep at top · up to 60 sweeps · tap a frequency to inspect.";
    if (shared.historyControl) shared.historyControl.disabled = !isLive();
    if (shared.pauseControl) shared.pauseControl.disabled = !isLive();
    el("status").textContent = isLive() ? liveFreqs.length ? `Live · ${liveFreqs.length} pilot channels · timing continues` : "Waiting for live pilot readings…" : "Ready to sweep. Pilot history will pause until stopped.";
    render();
  }
  for (const key of ["range", "step"]) el(key).onchange = selectView;
  for (const key of ["trace", "waterfall"]) el(key).onpointerdown = e => {
    if (key === "waterfall" && isLive()) return;
    if (!freqs.length) return;
    const box = el(key).getBoundingClientRect(), [lo, hi] = range();
    const target = lo + Math.max(0, Math.min(1, (e.clientX - box.left - 38) / (box.width - 50))) * (hi - lo);
    selected = freqs.reduce((best, f) => Math.abs(f - target) < Math.abs(best - target) ? f : best, freqs[0]);
    render();
  };
  link.addEventListener("unavailable", () => { unavailable = true; controller?.abort(); });
  const observer = new ResizeObserver(render); observer.observe(container);
  selectView();
  return {
    ownsWaterfall: () => !isLive(),
    stop: () => controller?.abort(),
    updateLive(entries) {
      if (!entries.length) return;
      const frequencies = [...new Set(entries.map(x => x.mhz))].sort((a, b) => a - b);
      if (frequencies.length !== liveFreqs.length || frequencies.some((f, i) => f !== liveFreqs[i])) {
        liveHistory = new SpectrumHistory(); liveFreqs = frequencies;
        lastLiveDraw = -Infinity;
        if (isLive()) { history = liveHistory; freqs = liveFreqs; selected = null; }
      }
      liveHistory.update(new Map(entries.map(x => [x.mhz, x.filt])));
      const now = performance.now();
      // Consume every incoming reading for peak hold, repaint at 20 fps.
      if (isLive() && now - lastLiveDraw >= 50) {
        lastLiveDraw = now;
        el("status").textContent = `Live · ${liveFreqs.length} pilot channels · timing continues`;
        render();
      }
    },
  };
}
