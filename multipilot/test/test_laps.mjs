// PC tests for the browser lap timer's pass detection and lap logic
// (web/laps.js), on synthetic readings at the C5's rate. Run: node test/test_laps.mjs
import { PassDetector, Session, SENSITIVITY, formatLap } from "../web/laps.js";

let failures = 0, checks = 0;
function check(ok, what) {
  checks++;
  if (!ok) { failures++; console.log("FAIL", what); }
}

// Repeatable noise.
let seed = 12345;
function rand() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
function gauss() { const u = Math.max(rand(), 1e-9), v = rand(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }

// A fly-by: the level over the floor at time t for a crossing at tc,
// free-space shape (speed m/s, closest distance m), like the rig's model.
function passLevel(t, tc, peakDb, speed = 20, d0 = 2) {
  const r = (t - tc) * speed / d0;
  return peakDb - 10 * Math.log10(1 + r * r);
}

// Readings at `rate` per second from t0 to t1; level(t) in dB, plus noise.
function run(det, t0, t1, level, { rate = 1000, noise = 0.6 } = {}) {
  const out = [];
  for (let t = t0; t < t1; t += 1 / rate) {
    const p = det.push(t, level(t) + noise * gauss());
    if (p) out.push(p);
  }
  return out;
}

// 1. Laps timed accurately at 1 kHz, medium sensitivity.
{
  const det = new PassDetector(SENSITIVITY.medium);
  const crossings = [5, 17.3, 29.05, 40.9, 52.6];
  const floor = 4;
  const level = t => floor + Math.max(0, ...crossings.map(c => passLevel(t, c, 28)));
  const passes = run(det, 0, 58, level).filter(p => !p.rejected);
  check(passes.length === crossings.length, `1: ${passes.length} passes, want ${crossings.length}`);
  passes.forEach((p, k) => check(Math.abs(p.t - crossings[k]) < 0.01, `1: pass ${k} at ${p.t.toFixed(4)}, want ${crossings[k]}`));
  for (let k = 1; k < passes.length; k++) {
    const want = crossings[k] - crossings[k - 1], got = passes[k].t - passes[k - 1].t;
    check(Math.abs(got - want) < 0.01, `1: lap ${k} ${got.toFixed(4)}, want ${want.toFixed(4)}`);
  }
  check(passes.every(p => p.peakDb > 20 && p.peakDb < 32), "1: peak over floor about 28 dB");
}

// 2. The same at the old 140 readings/s, and with 2 readings per record.
{
  const det = new PassDetector(SENSITIVITY.medium);
  const crossings = [5, 16, 27.5];
  const level = t => 4 + Math.max(0, ...crossings.map(c => passLevel(t, c, 25)));
  const passes = run(det, 0, 32, level, { rate: 140 }).filter(p => !p.rejected);
  check(passes.length === 3, `2: ${passes.length} passes at 140/s`);
  passes.forEach((p, k) => check(Math.abs(p.t - crossings[k]) < 0.02, `2: pass ${k} off by ${(p.t - crossings[k]).toFixed(4)} s`));
}

// 3. A weak pass is missed on Low and caught on High.
{
  const level = t => 4 + Math.max(0, passLevel(t, 5, 12));
  check(run(new PassDetector(SENSITIVITY.low), 0, 10, level).length === 0, "3: low ignores a 12 dB pass");
  check(run(new PassDetector(SENSITIVITY.high), 0, 10, level).length === 1, "3: high catches a 12 dB pass");
}

// 4. A crash through the gate 1 s after a lap is reported as too soon and
//    doesn't move the lap reference.
{
  const det = new PassDetector({ ...SENSITIVITY.medium, minLapS: 3 });
  const crossings = [5, 6, 17];
  const level = t => 4 + Math.max(0, ...crossings.map(c => passLevel(t, c, 25, 30, 1.5)));
  const passes = run(det, 0, 20, level);
  check(passes.length === 3, `4: ${passes.length} passes`);
  check(passes[1] && passes[1].rejected === "too-soon", "4: second pass too soon");
  check(passes[2] && !passes[2].rejected, "4: third pass counts");
}

// 5. A quad sat at the gate for 2.6-6 s is too long; after 8 s it becomes the
//    floor, and a real pass after it is still caught.
{
  const det = new PassDetector({ ...SENSITIVITY.medium, maxPassS: 2.5 });
  const level = t => 4 + (t > 3 && t < 7 ? 25 : 0) + passLevel(t, 12, 25) * (t > 9 ? 1 : 0);
  const passes = run(det, 0, 15, t => Math.max(4, level(t)));
  check(passes.length >= 1 && passes[0].rejected === "too-long", "5: parked quad rejected as too long");
  check(passes.some(p => !p.rejected && Math.abs(p.t - 12) < 0.02), "5: the pass after it counts");
  const det2 = new PassDetector(SENSITIVITY.medium);
  const parked = run(det2, 0, 20, t => (t > 2 ? 30 : 4));
  check(parked.length === 0 && !det2.inPass && det2.floor > 25, "5: parked 18 s: no pass, floor moved up to it");
}

// 6. The floor follows a slow drift (room warming up, a VTX powering up far
//    away) without false passes.
{
  const det = new PassDetector(SENSITIVITY.medium);
  const passes = run(det, 0, 60, t => 4 + t * 0.1);
  check(passes.length === 0, `6: drift gave ${passes.length} passes`);
}

// 7. Sessions: practice and race.
{
  const s = new Session({ mode: "practice", pilots: 1 });
  check(s.pass(0, 5).kind === "start", "7: practice starts on the first pass");
  const a = s.pass(0, 17);
  check(a.kind === "lap" && a.lap === 1 && Math.abs(a.lapS - 12) < 1e-9 && a.best, "7: lap 1");
  const b = s.pass(0, 28.5);
  check(b.lap === 2 && Math.abs(b.lapS - 11.5) < 1e-9 && b.best, "7: lap 2 best");
  const c = s.pass(0, 41);
  check(c.lap === 3 && !c.best, "7: lap 3 not best");

  const r = new Session({ mode: "race", laps: 2, pilots: 2 });
  check(r.pass(0, 1).kind === "ignored", "7: race ignores passes before GO");
  r.go(10);
  check(r.pass(0, 9).kind === "ignored", "7: and before the GO time");
  const h = r.pass(0, 11.5);
  check(h.kind === "holeshot" && Math.abs(h.lapS - 1.5) < 1e-9, "7: holeshot from GO");
  r.pass(1, 12);
  r.pass(0, 22);
  r.pass(1, 23);
  const f0 = r.pass(0, 33);
  check(f0.finished && Math.abs(f0.totalS - 23) < 1e-9, "7: pilot 0 finishes on lap 2");
  check(!r.finished(), "7: race not over until everyone is in");
  const f1 = r.pass(1, 34);
  check(f1.finished && r.finished(), "7: race over");
  check(r.pass(0, 40).kind === "ignored", "7: passes after finishing ignored");
  check(r.standings().join() === "0,1", "7: standings");
}

check(formatLap(12.345) === "12.35" && formatLap(75.5) === "1:15.50" && formatLap(null) === "–", "8: formatLap");

console.log(failures ? `${failures} of ${checks} checks failed` : `laps: ${checks} checks, 0 failed`);
process.exit(failures ? 1 : 0);
