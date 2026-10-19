// The VTX check, as a panel any page can mount: find which channel a quad's
// VTX is on (vtxscan.js), whether it's on frequency, and its power against a
// saved 25 mW reference. Used by the VTX check page (vtx/) and the dashboard.
//
// mountVtxCheck(container, link) needs a link with:
//   connected                      true while the C5 is connected
//   send(line), setChannels(list)  console commands
//   addEventListener("record")     records shaped as c5mk-link.js sends them
// and optionally beginCheck() / endCheck(), called around anything that
// retunes the C5 (the dashboard pauses its charts and puts its channels back).
import { BANDS, MIN_MHZ, MAX_MHZ, FLAG, channelName } from "./c5mk-link.js";
import { bandChannels, fill8, sweep, strongest, fineCentre, median } from "./vtxscan.js";

const REF_KEY = "c5mk.vtx.reference";   // shared by every page on the site
const BAND_ORDER = ["R", "F", "A", "B", "E", "L"];
const sleep = ms => new Promise(r => setTimeout(r, ms));

const CSS = `
.vx { display: grid; gap: 14px; }
.vx-card { background: var(--panel2, #1f232a); border: 1px solid var(--line, #2a2f37); border-radius: 14px; padding: 14px 16px; min-width: 0; }
.vx h3 { font-size: 12px; font-weight: 700; letter-spacing: .08em; text-transform: uppercase; color: var(--muted, #8a919c); margin: 0 0 10px; }
.vx p { margin: 0; color: var(--soft, #b9bfc9); }
.vx .vx-muted { color: var(--muted, #8a919c); font-size: 13px; }
.vx button { font: inherit; color: var(--text, #e8eaee); cursor: pointer; border: 1px solid var(--line2, #353b45); background: var(--panel, #191c21);
  border-radius: 10px; padding: 8px 14px; min-height: 44px; }
.vx button:disabled { opacity: .45; cursor: default; }
.vx button.vx-primary { background: var(--accent, #3987e5); border-color: var(--accent, #3987e5); color: #fff; font-weight: 700; width: 100%; font-size: 17px; min-height: 52px; }
.vx-notice { border-radius: 10px; padding: 10px 12px; font-size: 14px; background: #2c1717; border: 1px solid #6a2a2a; color: #f4b9b9; }
.vx-notice:empty { display: none; }
.vx [hidden] { display: none !important; }
.vx-progress { height: 6px; border-radius: 3px; background: #2b3039; overflow: hidden; margin-top: 12px; }
.vx-progress i { display: block; height: 100%; width: 0; background: var(--accent, #3987e5); transition: width .2s; }
.vx-found { display: grid; gap: 4px; }
.vx-found .vx-ch { font: 800 52px/1 var(--mono, ui-monospace, monospace); letter-spacing: -.02em; }
.vx-found .vx-mhz { font-size: 19px; color: var(--soft, #b9bfc9); }
.vx-verdict { display: inline-flex; align-items: center; gap: 8px; font-weight: 700; margin-top: 8px; padding: 5px 12px; border-radius: 999px; width: fit-content; }
.vx-verdict.ok { background: #12291e; color: #bff0d6; border: 1px solid #2a6a4b; }
.vx-verdict.warn { background: #2a2316; color: #f1d9a3; border: 1px solid #5a4823; }
.vx-verdict.bad { background: #2c1717; color: #f4b9b9; border: 1px solid #6a2a2a; }
.vx-grid { display: grid; grid-template-columns: 32px repeat(8, minmax(0, 1fr)); gap: 4px; font-size: 12px; }
.vx-grid .vx-band { color: var(--muted, #8a919c); font-weight: 700; display: grid; place-items: center; }
.vx-grid .vx-cell { border-radius: 6px; padding: 5px 0 4px; text-align: center; line-height: 1.15; border: 2px solid transparent; min-width: 0; }
.vx-grid .vx-cell b { display: block; font-size: 12px; }
.vx-grid .vx-cell span { font: 11px var(--mono, ui-monospace, monospace); opacity: .85; }
.vx-grid .vx-cell.win { border-color: #fff; }
.vx-grid .vx-cell.off { background: #1a1d22; color: #4e555f; }
.vx-legend { display: flex; align-items: center; gap: 8px; margin-top: 10px; font-size: 12px; color: var(--muted, #8a919c); }
.vx-legend i { flex: 1; display: flex; gap: 2px; height: 8px; }
.vx-legend i span { flex: 1; }
.vx-legend i span:nth-child(1) { background: #1b2230; }
.vx-legend i span:nth-child(2) { background: #185cab; }
.vx-legend i span:nth-child(3) { background: #39a0e5; }
.vx-legend i span:nth-child(4) { background: #b0d6fa; }
.vx-power { display: grid; gap: 10px; }
.vx-power .vx-big { font: 800 32px/1.1 var(--mono, ui-monospace, monospace); }
.vx-row { display: flex; gap: 10px; flex-wrap: wrap; }
.vx-row > button { flex: 1; }
`;

const HTML = `
<div class="vx-card" data-k="find">
  <h3>Find the channel</h3>
  <p>Power up one quad, about a metre from the receiver. Turn other video transmitters off.</p>
  <button class="vx-primary" data-k="check" style="margin-top:12px">Check VTX</button>
  <div class="vx-progress" data-k="prog" hidden><i></i></div>
</div>
<div class="vx-notice" data-k="notice"></div>
<div class="vx-card" data-k="result" hidden>
  <div class="vx-found">
    <div class="vx-muted">Your VTX is on</div>
    <div class="vx-ch" data-k="ch">–</div>
    <div class="vx-mhz" data-k="mhz"></div>
    <div class="vx-verdict" data-k="freq"></div>
  </div>
</div>
<div class="vx-card" data-k="gridbox" hidden>
  <h3>Every channel</h3>
  <div class="vx-grid" data-k="grid"></div>
  <div class="vx-legend"><span>quiet</span><i aria-hidden="true"><span></span><span></span><span></span><span></span></i><span>strong</span></div>
  <p class="vx-muted" style="margin-top:10px">Numbers are dB above the quietest channels. Strong neighbours of the winner are
    normal: a video signal is about 18 MHz wide.</p>
</div>
<div class="vx-card vx-power" data-k="power" hidden>
  <h3>Power check</h3>
  <div data-k="powerBody"></div>
  <div class="vx-row">
    <button data-k="measure">Measure this quad</button>
    <button data-k="ref">Use as 25 mW reference</button>
  </div>
  <p class="vx-muted">Put a quad you know is 25 mW at a marked spot (same distance, same way round) and save it as the
    reference. Then put each quad on the same spot. 200 mW reads about 9 dB higher, 600 mW about 14 dB. This compares against
    your reference; it isn't a calibrated power meter, and walls, hands and antenna angle all move the reading a dB or two.</p>
</div>`;

// Levels shaded like the dashboard's waterfall: one hue, dark to light.
function shade(x) {
  const stops = [[0, [27, 34, 48]], [0.4, [24, 92, 171]], [0.7, [57, 160, 229]], [1, [176, 214, 250]]];
  x = Math.max(0, Math.min(1, x));
  for (let i = 1; i < stops.length; i++) {
    const [b, cb] = stops[i], [a, ca] = stops[i - 1];
    if (x <= b) return `rgb(${ca.map((c, k) => Math.round(c + (cb[k] - c) * (x - a) / (b - a))).join(",")})`;
  }
  return "rgb(176,214,250)";
}

const loadRef = () => { try { return JSON.parse(localStorage.getItem(REF_KEY)); } catch (e) { return null; } };
const saveRef = r => { try { localStorage.setItem(REF_KEY, JSON.stringify(r)); } catch (e) {} };

export function mountVtxCheck(container, link) {
  if (!document.getElementById("vx-style")) {
    const st = document.createElement("style");
    st.id = "vx-style";
    st.textContent = CSS;
    document.head.appendChild(st);
  }
  container.classList.add("vx");
  container.innerHTML = HTML;
  const el = k => container.querySelector(`[data-k="${k}"]`);
  let busy = false, found = null, lastLevel = null, gain = null;

  const notice = text => { el("notice").textContent = text || ""; };
  const begin = () => link.beginCheck?.() !== false;
  const end = () => link.endCheck?.();

  function renderGrid(levels, winner) {
    const vals = [...levels.values()].filter(Number.isFinite);
    const base = median(vals), top = Math.max(base + 10, ...vals);
    let html = "";
    for (const b of BAND_ORDER) {
      html += `<div class="vx-band">${b}</div>`;
      BANDS[b].forEach((f, k) => {
        const db = levels.get(f);
        if (f < MIN_MHZ || f > MAX_MHZ || !Number.isFinite(db)) {
          html += `<div class="vx-cell off" title="${b}${k + 1} ${f} MHz: out of the receiver's range"><b>${b}${k + 1}</b><span>–</span></div>`;
          return;
        }
        const x = (db - base) / (top - base), light = x > 0.62;
        html += `<div class="vx-cell${f === winner ? " win" : ""}" style="background:${shade(x)};color:${light ? "#0b1220" : "#e8eaee"}"
          title="${b}${k + 1} ${f} MHz"><b>${b}${k + 1}</b><span>+${Math.max(0, db - base).toFixed(0)}</span></div>`;
      });
    }
    el("grid").innerHTML = html;
  }

  async function check() {
    if (busy) return;
    if (!link.connected) { notice("Connect the receiver first."); return; }
    if (!begin()) { notice("Stop the spectrum sweep before checking a VTX."); return; }
    busy = true;
    el("check").disabled = true;
    el("prog").hidden = false;
    notice("");
    const bar = el("prog").firstElementChild;
    try {
      const freqs = bandChannels(BAND_ORDER);
      const levels = await sweep(link, freqs, { onProgress: (d, n) => { bar.style.width = `${70 * d / n}%`; } });
      const best = strongest(levels);
      el("gridbox").hidden = false;
      renderGrid(levels, best.mhz);
      if (best.mhz === null || best.aboveDb < 10) {
        el("result").hidden = true;
        el("power").hidden = true;
        found = null;
        notice("No VTX found. Is the quad powered up with its video on? Some VTXs stay in pit mode (very low power) until armed: bring it right up to the receiver, or check its settings.");
        return;
      }
      bar.style.width = "75%";
      const fine = await fineCentre(link, best.mhz);
      bar.style.width = "90%";
      const centre = Number.isFinite(fine.centre) ? fine.centre : best.mhz;
      // The channel nearest the measured centre (the coarse winner can be a
      // neighbour when a VTX sits between channels).
      let nearest = best.mhz, d = Infinity;
      for (const f of freqs) if (Math.abs(f - centre) < d) { d = Math.abs(f - centre); nearest = f; }
      // Power is measured where the VTX actually is, not on the channel: one a
      // few MHz off would otherwise read weaker than it is.
      const at = Math.min(MAX_MHZ, Math.max(MIN_MHZ, Math.round(centre)));
      found = { mhz: nearest, centre, at };
      const off = centre - nearest;
      el("ch").textContent = channelName(nearest);
      const names = BAND_ORDER.flatMap(b => BANDS[b].map((f, k) => f === nearest ? b + (k + 1) : null)).filter(Boolean);
      el("mhz").textContent = `${nearest} MHz` + (names.length > 1 ? ` (also ${names.slice(1).join(", ")})` : "");
      const v = el("freq");
      if (Math.abs(off) <= 3) { v.className = "vx-verdict ok"; v.textContent = "✓ On frequency"; }
      else if (Math.abs(off) <= 6) { v.className = "vx-verdict warn"; v.textContent = `${off > 0 ? "+" : "−"}${Math.abs(off).toFixed(0)} MHz from the channel: probably fine`; }
      else { v.className = "vx-verdict bad"; v.textContent = `${off > 0 ? "+" : "−"}${Math.abs(off).toFixed(0)} MHz off the channel: check the VTX settings`; }
      el("result").hidden = false;
      renderGrid(levels, nearest);
      await measureLevel();
      bar.style.width = "100%";
    } finally {
      end();
      busy = false;
      el("check").disabled = false;
      el("check").textContent = "Check again";
      setTimeout(() => { el("prog").hidden = true; bar.style.width = "0"; }, 400);
    }
  }

  // The found VTX's level: the median of 2 s of raw readings at its centre.
  async function measureLevel() {
    el("measure").disabled = el("ref").disabled = true;
    const vals = [];
    let clipped = false, t0 = Infinity;
    const onRecord = e => {
      if (performance.now() < t0) return;
      for (const x of e.detail.entries) {
        if (x.mhz !== found.at) continue;
        if (Number.isFinite(x.raw)) vals.push(x.raw);
        if (x.flags & FLAG.CLIP) clipped = true;
        gain = x.gain;
      }
    };
    link.addEventListener("record", onRecord);
    await link.setChannels(fill8([found.at]));
    t0 = performance.now() + 150;
    el("powerBody").innerHTML = `<p class="vx-muted">Measuring… hold the quad still.</p>`;
    el("power").hidden = false;
    await sleep(2150);
    link.removeEventListener("record", onRecord);
    lastLevel = { db: median(vals), gain, clipped, mhz: found.at };
    el("measure").disabled = false;
    renderPower();
  }

  async function measureAgain() {
    if (busy || !found) return;
    if (!link.connected) { notice("Connect the receiver first."); return; }
    if (!begin()) { notice("Stop the spectrum sweep before measuring a VTX."); return; }
    busy = true;
    try { await measureLevel(); } finally { end(); busy = false; }
  }

  function renderPower() {
    const ref = loadRef(), L = lastLevel;
    let html = "";
    if (!L || !Number.isFinite(L.db)) {
      html = `<p class="vx-muted">No reading. Press Measure.</p>`;
    } else if (L.clipped) {
      html = `<div class="vx-verdict bad">Too close</div><p>The receiver is overloaded, so the reading isn't trustworthy.
        Move the quad further away (and use the same spot for the reference).</p>`;
    } else if (!ref) {
      html = `<p>Level <b>${L.db.toFixed(1)} dB</b>. To turn this into a power check, measure a quad you know is 25 mW on
        the same spot and press <b>Use as 25 mW reference</b>.</p>`;
    } else {
      const delta = L.db - ref.db, mw = 25 * 10 ** (delta / 10);
      const est = mw < 10 ? mw.toFixed(1) : mw < 100 ? mw.toFixed(0) : String(Math.round(mw / 10) * 10);
      let cls, word;
      if (delta <= 3) { cls = "ok"; word = delta < -6 ? "Below 25 mW (or further away than the reference)" : "OK for 25 mW racing"; }
      else if (delta <= 6) { cls = "warn"; word = "A bit high: check the VTX power setting"; }
      else { cls = "bad"; word = "Too high for 25 mW racing"; }
      html = `<div class="vx-big">${delta > -0.05 ? "+" : "−"}${Math.abs(delta).toFixed(1)} dB <span class="vx-muted" style="font:15px system-ui">vs reference</span></div>
        <div>About <b>${est} mW</b> if the reference is 25 mW and the quad is on the same spot.</div>
        <div class="vx-verdict ${cls}">${word}</div>`;
      const notes = [];
      if (ref.gain != null && L.gain != null && ref.gain !== L.gain) notes.push(`The reference was measured at receiver gain ${ref.gain}, this at ${L.gain}: measure the reference again.`);
      if (Math.abs(ref.mhz - L.mhz) > 120) notes.push(`The reference was on ${channelName(ref.mhz)} and this is on ${channelName(L.mhz)}: far-apart channels can read a dB or two differently.`);
      notes.push(`Reference: ${ref.db.toFixed(1)} dB on ${channelName(ref.mhz)}, saved ${new Date(ref.at).toLocaleDateString()}.`);
      html += notes.map(n => `<p class="vx-muted">${n}</p>`).join("");
    }
    el("powerBody").innerHTML = html;
    el("ref").disabled = !L || !Number.isFinite(L.db) || L.clipped;
  }

  el("ref").onclick = () => {
    const L = lastLevel;
    if (!L || !Number.isFinite(L.db) || L.clipped) return;
    if (loadRef() && !confirm("Replace the saved 25 mW reference with this quad?")) return;
    saveRef({ db: L.db, gain: L.gain, mhz: L.mhz, at: Date.now() });
    renderPower();
  };
  el("measure").onclick = measureAgain;
  el("check").onclick = check;
  return { check };
}
