import assert from "node:assert/strict";
import { RssiHistory, RssiGraph } from "../web/rssi-graph.js";
import { PassDetector } from "../web/laps.js";
import { SENSITIVITY } from "../web/laps.js";
import { readFileSync } from "node:fs";
import vm from "node:vm";

// Bucketing must retain a short peak even if it falls before the next repaint.
const history = new RssiHistory();
history.push(1, 2);
history.push(1.001, 45);
history.push(1.002, 3, 1.001);
assert.equal(history.buckets.length, 1);
assert.equal(history.buckets[0].max, 45);
assert.equal(history.buckets[0].min, 2);
assert.deepEqual(history.passes, [1.001]);
history.push(2, NaN);
assert.equal(history.buckets.length, 1);
for (let n = 1003; n < 61000; n++) history.push(n / 1000, 3);
assert.ok(history.buckets.length <= 1002);
assert.ok(history.buckets[0].lastT >= 60.999 - 20);
assert.equal(history.passes.length, 0);
history.push(0, 4);
assert.equal(history.buckets.length, 1, "clock restart discards the previous history");

// A real detector and graph agree about accepted pass markers and custom values.
const ctx = new Proxy({}, { get: (obj, key) => obj[key] ?? (() => {}) });
const canvas = { clientWidth: 280, clientHeight: 150, getContext: () => ctx, setAttribute() {} };
const output = {};
const graph = new RssiGraph(canvas, output, "#3987e5");
const detector = new PassDetector({ enterDb: 12, exitDb: 4 });
for (let n = 0; n < 5000; n++) {
  const t = n / 1000;
  const pass = detector.push(t, 4 + (t > 2 && t < 2.3 ? 25 : 0));
  graph.push(t, detector.above(), pass && !pass.rejected ? pass.t : null);
}
assert.equal(graph.passes.length, 1);
graph.draw(5, detector);
assert.match(output.textContent, /dB/);
graph.draw(7, detector);
assert.equal(output.textContent, "No recent signal");
graph.draw(30, detector);
canvas.clientWidth = 0;
graph.draw(30, detector);

// Exercise pointer capture and keyboard changes against the timer's real
// threshold update code, including keeping typed values in sync with dragging.
const page = readFileSync(new URL("../web/timer/index.html", import.meta.url), "utf8");
const cfg = { sens: "medium", customThresholds: true, minLapS: 3, pilots: [{ enterDb: 14, exitDb: 6 }] };
const inputs = ["enterDb", "exitDb"].map(key => ({ dataset: {key} }));
const fields = { querySelectorAll: () => inputs };
let saves = 0;
const context = vm.createContext({ cfg, detectors: [detector], SENSITIVITY,
  save: () => saves++, document: {activeElement: null},
  $: () => ({children: [{querySelector: () => fields}]}),
});
vm.runInContext(page.slice(page.indexOf("function thresholds("), page.indexOf("function buildGraphs(")), context);
const change = (key, value) => context.updateThreshold(0, key, value);
const learnedFloor = detector.floor;
change("enterDb", 14);
function handle(key) {
  return { dataset: {key}, style: {}, attributes: {}, captured: null,
    setAttribute(name, value) { this.attributes[name] = value; },
    setPointerCapture(id) { this.captured = id; },
    hasPointerCapture(id) { return this.captured === id; },
    releasePointerCapture() { this.captured = null; },
  };
}
const entryHandle = handle("enterDb"), exitHandle = handle("exitDb");
const touchCanvas = {...canvas, clientWidth: 280, clientHeight: 220, getBoundingClientRect: () => ({top:100})};
let enabled = true;
const draggable = new RssiGraph(touchCanvas, {}, "#3987e5", {
  handles: [entryHandle, exitHandle], enabled: () => enabled, change,
});
draggable.draw(5, detector);
const event = y => ({ pointerId: 42, isPrimary: true, button: 0, clientY: y, preventDefault() {} });
entryHandle.onpointerdown(event(100 + parseFloat(entryHandle.style.top)));
assert.equal(entryHandle.captured, 42);
const scale = draggable.geometry.top;
const y20 = 100 + draggable.geometry.bottom - 20 / scale * (draggable.geometry.bottom - draggable.geometry.inset);
entryHandle.onpointermove(event(y20));
assert.equal(detector.enterDb, 20);
assert.equal(inputs[0].value, 20);
assert.equal(draggable.geometry.top, scale, "scale stays fixed under the finger");
entryHandle.onpointermove(event(10000));
assert.equal(detector.enterDb, 6.5, "entry cannot be dragged below exit");
entryHandle.onpointermove(event(-10000));
assert.equal(detector.enterDb, scale, "dragging stays within the visible plot");
entryHandle.onpointercancel(event(0));
assert.equal(draggable.drag, null);
assert.equal(entryHandle.captured, null);
change("enterDb", 100);
assert.equal(detector.enterDb, 80, "typed values obey the upper limit");
draggable.draw(5, detector);
exitHandle.onpointerdown(event(100 + parseFloat(exitHandle.style.top)));
exitHandle.onpointermove(event(-10000));
assert.equal(detector.exitDb, 79.5, "exit cannot be dragged above entry");
exitHandle.onpointerup(event(0));
entryHandle.onkeydown({key:"ArrowDown", preventDefault() {}});
assert.equal(detector.enterDb, 80, "keyboard obeys threshold separation");
exitHandle.onkeydown({key:"ArrowDown", shiftKey:true, preventDefault() {}});
assert.equal(detector.exitDb, 74.5);
enabled = false;
entryHandle.onpointerdown(event(150));
assert.equal(draggable.drag, null, "racing and preset mode disable dragging");
assert.ok(saves > 0);
assert.equal(detector.floor, learnedFloor, "editing keeps the learned noise floor");
console.log("RSSI graph: history, rendering, touch drag, capture cancellation, limits, keyboard and input sync passed");
