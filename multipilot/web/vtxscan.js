// Finding a VTX: the C5 is moved across the band eight frequencies at a time,
// and each frequency's level is the median of its raw readings (median: a
// Wi-Fi burst or a capture glitch doesn't move it). Used by the lap timer's
// "Find my quad" and the VTX check page.
import { BANDS, MIN_MHZ, MAX_MHZ } from "./c5mk-link.js";

const sleep = ms => new Promise(r => setTimeout(r, ms));

export function median(a) {
  if (!a.length) return NaN;
  const s = [...a].sort((x, y) => x - y), m = s.length >> 1;
  return s.length & 1 ? s[m] : (s[m - 1] + s[m]) / 2;
}

// Every channel of these bands the C5 can tune, once each, low to high.
export function bandChannels(bands = ["R", "A", "B", "E", "F"]) {
  const set = new Set();
  for (const b of bands) for (const f of BANDS[b]) if (f >= MIN_MHZ && f <= MAX_MHZ) set.add(f);
  return [...set].sort((a, b) => a - b);
}

// The C5 always gets eight entries: shorter lists are repeated, so its
// records stay the size and rate it's measured at.
export function fill8(list) {
  const out = [];
  for (let i = 0; i < 8; i++) out.push(list[i % list.length]);
  return out;
}

// Measures each frequency. Returns Map(mhz -> level dB, NaN if no readings).
// settleMs: readings ignored after each retune; dwellMs: readings kept.
// onProgress(done, total). Leaves the C5 on the last group.
export async function sweep(link, freqs, { settleMs = 120, dwellMs = 250, onProgress, signal } = {}) {
  const levels = new Map();
  for (let g = 0; g < freqs.length; g += 8) {
    if (signal?.aborted) break;
    if (!link.connected) throw new Error("Receiver disconnected");
    const group = freqs.slice(g, g + 8);
    const got = new Map(group.map(f => [f, []]));
    let t0 = Infinity;
    const onRecord = e => {
      if (performance.now() < t0) return;
      for (const x of e.detail.entries) {
        const a = got.get(x.mhz);
        if (a && Number.isFinite(x.raw)) a.push(x.raw);
      }
    };
    link.addEventListener("record", onRecord);
    try {
      await link.setChannels(fill8(group));
      t0 = performance.now() + settleMs;
      await sleep(settleMs + dwellMs);
      if (!link.connected) throw new Error("Receiver disconnected");
    } finally {
      link.removeEventListener("record", onRecord);
    }
    if (signal?.aborted) break;
    for (const [f, a] of got) levels.set(f, median(a));
    onProgress?.(Math.min(g + 8, freqs.length), freqs.length, levels);
  }
  return levels;
}

// The strongest frequency, and how far it stands above the rest (the median
// of all levels), dB.
export function strongest(levels) {
  let best = null, bestDb = -Infinity;
  const all = [];
  for (const [f, db] of levels) {
    if (!Number.isFinite(db)) continue;
    all.push(db);
    if (db > bestDb) { bestDb = db; best = f; }
  }
  return { mhz: best, db: bestDb, aboveDb: best === null ? 0 : bestDb - median(all) };
}

// A finer look around one frequency (2 MHz steps over +-14 MHz): the VTX's
// centre, as the power-weighted middle of the points within 6 dB of the top.
export async function fineCentre(link, mhz, opts = {}) {
  const freqs = [];
  for (let d = -14; d <= 14; d += 2) {
    const f = mhz + d;
    if (f >= MIN_MHZ && f <= MAX_MHZ) freqs.push(f);
  }
  const levels = await sweep(link, freqs, opts);
  const pts = [...levels].filter(([, db]) => Number.isFinite(db));
  if (!pts.length) return { centre: NaN, levels };
  const top = Math.max(...pts.map(([, db]) => db));
  let sw = 0, sf = 0;
  for (const [f, db] of pts) {
    if (db < top - 6) continue;
    const w = 10 ** (db / 10);
    sw += w;
    sf += w * f;
  }
  return { centre: sf / sw, levels };
}
