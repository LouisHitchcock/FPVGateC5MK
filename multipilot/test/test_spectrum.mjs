import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { spectrumFrequencies, SpectrumHistory, mountSpectrum } from "../web/spectrum.js";
import { sweep } from "../web/vtxscan.js";

for (const step of [2, 5, 10]) {
  const freqs = spectrumFrequencies(5180, 5917, step);
  assert.equal(freqs[0], 5180);
  assert.equal(freqs.at(-1), 5917);
  assert.equal(new Set(freqs).size, freqs.length);
  assert.ok(freqs.every((f, i) => !i || f > freqs[i-1]));
}
assert.throws(() => spectrumFrequencies(5180, 5918, 5));
assert.throws(() => spectrumFrequencies(5180, 5917, 0));
const history = new SpectrumHistory();
history.update(new Map([[5658, 10], [5732, NaN]]));
history.complete(1);
history.update(new Map([[5658, 5], [5732, 15]]));
assert.equal(history.peaks.get(5658), 10);
assert.equal(history.levels.get(5658), 5);
assert.equal(history.rows[0].levels.get(5658), 10, "waterfall rows are independent snapshots");
for (let i = 0; i < 70; i++) history.complete(i + 2);
assert.equal(history.rows.length, 60);
assert.equal(history.rows[0].at, 71);

class FakeLink extends EventTarget {
  connected = true; groups = []; listeners = 0; emitDelay = 5;
  addEventListener(...args) { this.listeners++; super.addEventListener(...args); }
  removeEventListener(...args) { this.listeners--; super.removeEventListener(...args); }
  async setChannels(group) {
    this.groups.push(group);
    setTimeout(() => this.dispatchEvent(new CustomEvent("record", { detail: {
      entries: group.flatMap(mhz => [2, 4, 100].map(raw => ({mhz, raw}))),
    } })), this.emitDelay);
  }
}
const link = new FakeLink(), freqs = spectrumFrequencies(5645, 5700, 5);
let progress = 0;
const measured = await sweep(link, freqs, {settleMs:0, dwellMs:20, onProgress: (done, total, partial) => {
  assert.equal(total, freqs.length);
  assert.equal(partial.size, done);
  progress = done;
}});
assert.equal(progress, freqs.length);
assert.equal(measured.size, freqs.length);
assert.ok([...measured.values()].every(db => db === 4), "spectrum uses median readings");
assert.equal(link.groups.length, 2);
assert.ok(link.groups.every(g => g.length === 8));
assert.equal(link.listeners, 0);
const abort = new AbortController(), stopped = new FakeLink();
const partial = await sweep(stopped, freqs, {settleMs:0, dwellMs:20, signal:abort.signal, onProgress: () => abort.abort()});
assert.equal(partial.size, 8);
assert.equal(stopped.groups.length, 1, "stopping does not retune another group");
assert.equal(stopped.listeners, 0);
const disconnected = new FakeLink();
disconnected.setChannels = async () => { disconnected.connected = false; };
await assert.rejects(sweep(disconnected, freqs, {settleMs:0,dwellMs:1}), /disconnected/);
assert.equal(disconnected.listeners, 0);
const rejected = new FakeLink();
rejected.setChannels = async () => { throw new Error("write failed"); };
await assert.rejects(sweep(rejected, freqs), /write failed/);
assert.equal(rejected.listeners, 0, "failed writes clean up the record listener");

// Test the dashboard's actual ownership adapters: no overlapping checks,
// channel restoration, and no spectrum takeover from an FPVGate S3.
const page = readFileSync(new URL("../web/index.html", import.meta.url), "utf8");
const controls = [{disabled:false}], elements = {vtxMount:{},vtxSub:{}};
const sent = [];
const context = vm.createContext({ EventTarget, port:{}, pilots:[{mhz:5658},{mhz:5732}],
  mode:"scan", scanning:true, performance:{now:()=>100},
  document:{querySelectorAll:()=>controls}, $:id=>elements[id], send:async line=>sent.push(line),
});
vm.runInContext(page.slice(page.indexOf("const vtxLink ="), page.indexOf("// Always on the dashboard")) + "\nglobalThis.adapters = {vtxLink,spectrumLink};", context);
const {spectrumLink, vtxLink} = context.adapters;
assert.equal(spectrumLink.beginCheck(), true);
assert.equal(controls[0].disabled, true);
assert.equal(elements.vtxMount.inert, true);
assert.equal(vtxLink.beginCheck(), false);
assert.equal(spectrumLink.beginCheck(), false);
await spectrumLink.endCheck();
assert.equal(sent.at(-1), "pilots 5658,5732");
assert.equal(controls[0].disabled, false);
assert.equal(elements.vtxMount.inert, false);
context.mode = "node";
assert.equal(spectrumLink.beginCheck(), false);
context.mode = "scan"; context.scanning = false;
assert.equal(spectrumLink.beginCheck(), false);
context.scanning = true;
assert.equal(vtxLink.beginCheck(), true);
assert.equal(spectrumLink.beginCheck(), false);
vtxLink.endCheck();
assert.equal(spectrumLink.beginCheck(), true);
context.port = null;
const count = sent.length;
await spectrumLink.endCheck();
assert.equal(sent.length, count, "disconnect never restores channels to another receiver");

// Mount and run the actual spectrum UI at phone canvas widths with a fake
// receiver. Canvas calls are recorded; this does not replace visual browser QA.
let waterfallPixels = 0;
const drawing = new Proxy({}, {get: (object, key) => object[key] ?? (key === "fillRect" ? () => waterfallPixels++ : () => {})});
const uiElements = Object.fromEntries(["start","range","step","hold","clear","status","trace","readout","waterfall","note","waterfallNote"].map(key => [key, {
  value: key === "range" ? "fpv" : key === "step" ? "10" : "",
  checked:true, clientWidth:280, clientHeight:key === "waterfall" ? 210 : 250,
  parentElement:{},
  getContext:()=>drawing, getBoundingClientRect:()=>({left:0,width:280}),
}]));
let ui, uiStatus = "", restored = 0, claimed = 0;
Object.defineProperty(uiElements.status, "textContent", {
  get:()=>uiStatus,
  set:value=> { uiStatus = value; if (/Sweep 1 complete/.test(value)) ui.stop(); },
});
globalThis.document = {getElementById:()=>null, createElement:()=>({}), head:{appendChild() {}}};
globalThis.ResizeObserver = class {observe(callback) {} };
const uiReceiver = new FakeLink(); uiReceiver.emitDelay = 150;
uiReceiver.beginCheck = () => { claimed++; return true; };
uiReceiver.endCheck = () => { restored++; };
const sharedHistory = {}, sharedPause = {};
let waterfallFits = 0;
uiElements.waterfall.getContext = () => { waterfallFits++; return drawing; };
const spectrumContainer = {querySelector: selector=>uiElements[selector.match(/data-sp="(.*?)"/)[1]]};
ui = mountSpectrum(spectrumContainer, uiReceiver, {
  waterfall:uiElements.waterfall, waterfallNote:uiElements.waterfallNote,
  historyControl:sharedHistory, pauseControl:sharedPause,
});
assert.equal((spectrumContainer.innerHTML.match(/<canvas/g) || []).length, 1, "spectrum only creates its trace, never a duplicate waterfall");
uiElements.range.value = "live";
uiElements.range.onchange();
assert.equal(ui.ownsWaterfall(), false, "live waterfall remains rendered by the dashboard");
assert.equal(sharedHistory.disabled, false);
const liveWaterfallFits = waterfallFits;
const live = [5658,5695,5732,5769,5806,5843,5880,5917].map((mhz,i)=>({mhz,filt:10+i}));
ui.updateLive(live);
assert.match(uiStatus, /Live · 8 pilot channels/);
assert.match(uiElements.readout.textContent, /5917 MHz · 17.0 dB/);
assert.equal(uiElements.start.hidden, true);
assert.equal(uiElements.step.parentElement.hidden, true);
assert.equal(claimed, 0);
assert.equal(uiReceiver.groups.length, 0, "live graph never retunes the timing receiver");
ui.updateLive(live.map((x,i)=>({...x,filt:i===0?75:x.filt})));
ui.updateLive(live);
uiElements.trace.onpointerdown({clientX:50});
assert.match(uiElements.readout.textContent, /5658 MHz.*peak 75.0 dB/, "peak hold consumes readings between repaints");
ui.updateLive([{mhz:5806,filt:12}]);
uiElements.hold.onchange();
assert.match(uiElements.readout.textContent, /5806 MHz · 12.0 dB/);
assert.equal(waterfallFits, liveWaterfallFits, "live spectrum never clears or paints the shared waterfall");
uiElements.range.value = "fpv";
uiElements.range.onchange();
assert.equal(ui.ownsWaterfall(), true, "sweeps use the same large waterfall canvas");
assert.equal(sharedHistory.disabled, true);
assert.equal(sharedPause.disabled, true);
await uiElements.start.onclick();
assert.equal(claimed, 1);
assert.equal(restored, 1);
assert.equal(uiElements.start.textContent, "Start sweep");
assert.equal(uiElements.range.disabled, false);
assert.match(uiStatus, /original channels restored/);
assert.match(uiElements.readout.textContent, /Strongest:.*4.0 dB/);
assert.ok(waterfallPixels > 0, "completed sweeps draw the frequency waterfall");
uiElements.trace.onpointerdown({clientX:150});
assert.match(uiElements.readout.textContent, /MHz.*4.0 dB.*peak/);
uiElements.range.onchange();
assert.equal(uiElements.readout.textContent, "No sweep yet");
uiReceiver.connected = false;
await uiElements.start.onclick();
assert.equal(claimed, 1, "disconnected UI does not claim the receiver");
assert.match(uiStatus, /Connect the receiver first/);
delete globalThis.document;
delete globalThis.ResizeObserver;
console.log("Spectrum: full band, peaks, waterfall history, measured sweeps, cancellation, disconnect, ownership and channel restoration passed");
