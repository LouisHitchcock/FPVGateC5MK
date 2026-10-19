// Gate-pass detection and lap counting for the browser lap timer. No browser
// APIs, so the PC tests run it under Node (multipilot/test/test_laps.mjs).
//
// One PassDetector per pilot, fed that pilot's readings (C5 clock, dB):
//   - smoothing: a time-based EMA (smoothS), so the reading rate doesn't matter;
//   - the floor follows the signal down quickly, creeps up at most
//     RISE_DB_PER_S, and holds still during a pass, so no one has to set it;
//   - a pass starts when the smoothed level is enterDb over the floor and ends
//     when it falls back to exitDb over it;
//   - its time is the middle of the stretch within plateauDb of its peak (the
//     top of a pass can be flat or bumpy; the middle of it is steadier than the
//     single highest reading), on the C5's clock, so USB delays don't matter;
//   - rejected: a pass sooner than minLapS after the last lap (a crash or a
//     bounce through the gate) or longer than maxPassS (a quad sat at the
//     gate). A quad that stays for PARK_RESET_S becomes the new floor.

export const SENSITIVITY = {
  low: { enterDb: 16, exitDb: 8, label: "Low", hint: "Strong passes only. For a timer right by the gate." },
  medium: { enterDb: 10, exitDb: 5, label: "Medium", hint: "Most setups." },
  high: { enterDb: 6, exitDb: 3, label: "High", hint: "Weak signals: low-power VTX or a timer further away." },
};

const FALL_S = 0.5;          // floor's time constant on the way down
const RISE_DB_PER_S = 0.5;   // floor's fastest climb
const PARK_RESET_S = 8;      // in a pass this long: a parked quad, start again
const MAX_SAMPLES = 20000;   // ~20 s at 1 kHz: bounds the pass buffer

export class PassDetector {
  constructor(opts = {}) {
    this.configure(opts);
    this.reset();
  }

  configure({ enterDb = 10, exitDb = 5, smoothS = 0.02, minLapS = 3, maxPassS = 2.5, plateauDb = 2 } = {}) {
    Object.assign(this, { enterDb, exitDb: Math.min(exitDb, enterDb - 0.5), smoothS, minLapS, maxPassS, plateauDb });
  }

  reset() {
    this.v = null;           // smoothed level, dB
    this.floor = null;
    this.lastT = null;
    this.inPass = false;
    this.samples = [];
    this.peak = -Infinity;
    this.start = 0;
    this.lastLapT = null;    // last counted pass
  }

  // Forget the lap reference (a new session), keep the floor.
  newSession() { this.lastLapT = null; }

  above() { return this.v === null ? 0 : this.v - this.floor; }

  // One reading. Returns null, or a pass: { t, peakDb (over the floor),
  // durationS, rejected: undefined | "too-soon" | "too-long" }.
  push(t, db) {
    if (!Number.isFinite(db)) return null;
    if (this.v === null) {
      this.v = this.floor = db;
      this.lastT = t;
      return null;
    }
    const dt = Math.min(0.5, Math.max(0, t - this.lastT));
    this.lastT = t;
    this.v += (1 - Math.exp(-dt / this.smoothS)) * (db - this.v);

    if (!this.inPass) {
      if (this.v < this.floor) this.floor += (this.v - this.floor) * Math.min(1, dt / FALL_S);
      else this.floor += Math.min(this.v - this.floor, RISE_DB_PER_S * dt);
      if (this.v - this.floor >= this.enterDb) {
        this.inPass = true;
        this.start = t;
        this.peak = this.v;
        this.samples = [[t, this.v]];
      }
      return null;
    }

    if (this.samples.length < MAX_SAMPLES) this.samples.push([t, this.v]);
    if (this.v > this.peak) this.peak = this.v;
    if (t - this.start > PARK_RESET_S) {
      // Parked by the gate: this is the room now.
      this.inPass = false;
      this.floor = this.v;
      this.samples = [];
      return null;
    }
    if (this.v - this.floor > this.exitDb) return null;

    this.inPass = false;
    const top = this.peak - this.plateauDb;
    let t1 = null, t2 = null;
    for (const [ts, vs] of this.samples) {
      if (vs >= top) { if (t1 === null) t1 = ts; t2 = ts; }
    }
    // The smoothing delays the signal by about its time constant: take it off,
    // so a pass is timed when the quad was at the gate (it cancels out of lap
    // times anyway, but a race's holeshot is timed from GO).
    const pass = { t: (t1 + t2) / 2 - this.smoothS, peakDb: this.peak - this.floor, durationS: t - this.start };
    this.samples = [];
    if (pass.durationS > this.maxPassS) pass.rejected = "too-long";
    else if (this.lastLapT !== null && pass.t - this.lastLapT < this.minLapS) pass.rejected = "too-soon";
    else this.lastLapT = pass.t;
    return pass;
  }
}

// Laps for one session of 1-4 pilots.
//   practice: a pilot's first pass starts their lap 1; laps until stopped.
//   race: passes before GO don't count; the first after GO is the holeshot
//   (lap 0, timed from GO); a pilot finishes on lap `laps`.
export class Session {
  constructor({ mode = "practice", laps = 3, pilots = 1 } = {}) {
    this.mode = mode;
    this.lapsToWin = laps;
    this.goT = null;   // race start, C5 clock
    this.pilots = Array.from({ length: pilots }, () => ({ lastT: null, laps: [], best: null, finished: false, finishT: null }));
  }

  go(tChip) { this.goT = tChip; }

  finished() {
    return this.mode === "race" && this.pilots.every(p => p.finished);
  }

  // A counted pass for pilot i. Returns what happened, for the screen and the
  // voice: { kind: "start" | "holeshot" | "lap" | "ignored", lap, lapS, best,
  // finished, totalS }.
  pass(i, t) {
    const p = this.pilots[i];
    if (this.mode === "race") {
      if (this.goT === null || t < this.goT || p.finished) return { kind: "ignored" };
      if (p.lastT === null) {
        p.lastT = t;
        return { kind: "holeshot", lap: 0, lapS: t - this.goT };
      }
    } else if (p.lastT === null) {
      p.lastT = t;
      return { kind: "start", lap: 0 };
    }
    const lapS = t - p.lastT;
    p.lastT = t;
    p.laps.push(lapS);
    const best = p.best === null || lapS < p.best;
    if (best) p.best = lapS;
    const lap = p.laps.length;
    let finished = false, totalS = null;
    if (this.mode === "race" && lap >= this.lapsToWin) {
      p.finished = finished = true;
      p.finishT = t;
      totalS = t - this.goT;
    }
    return { kind: "lap", lap, lapS, best, finished, totalS };
  }

  // Finishing order for a race: by laps, then by finish time.
  standings() {
    return this.pilots
      .map((p, i) => ({ i, laps: p.laps.length, at: p.finishT ?? p.lastT ?? Infinity }))
      .sort((a, b) => b.laps - a.laps || a.at - b.at)
      .map(r => r.i);
  }
}

export function formatLap(s) {
  if (s === null || s === undefined || !Number.isFinite(s)) return "–";
  if (s >= 60) {
    const m = Math.floor(s / 60);
    return `${m}:${(s - 60 * m).toFixed(2).padStart(5, "0")}`;
  }
  return s.toFixed(2);
}
