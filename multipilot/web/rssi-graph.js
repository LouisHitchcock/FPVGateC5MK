// Twenty seconds at 50 buckets/s. Keep each bucket's envelope so short gate
// peaks remain visible even when the receiver supplies thousands of samples/s.
const WINDOW_S = 20, BUCKET_S = 0.02, CAPACITY = 1002;

export class RssiHistory {
  constructor() { this.buckets = []; this.passes = []; }
  push(t, value, passT = null) {
    if (!Number.isFinite(t) || !Number.isFinite(value)) return;
    const last = this.buckets.at(-1);
    if (last && t < last.lastT) { this.buckets = []; this.passes = []; }
    const key = Math.floor(t / BUCKET_S), bucket = this.buckets.at(-1);
    if (bucket && bucket.key === key) {
      bucket.min = Math.min(bucket.min, value);
      bucket.max = Math.max(bucket.max, value);
      bucket.value = value;
      bucket.lastT = t;
    } else {
      this.buckets.push({ key, t, lastT: t, min: value, max: value, value });
    }
    if (Number.isFinite(passT)) this.passes.push(passT);
    while (this.buckets.length && (this.buckets[0].lastT < t - WINDOW_S || this.buckets.length > CAPACITY)) this.buckets.shift();
    while (this.passes.length && (this.passes[0] < t - WINDOW_S || this.passes.length > CAPACITY)) this.passes.shift();
  }
}

export class RssiGraph extends RssiHistory {
  constructor(canvas, output, color, editing = {}) {
    super();
    Object.assign(this, { canvas, output, color, editing });
    for (const handle of editing.handles || []) {
      const key = handle.dataset.key;
      handle.setAttribute("role", "slider");
      handle.setAttribute("aria-orientation", "vertical");
      handle.onpointerdown = e => {
        if (!editing.enabled?.() || !this.geometry || this.drag || e.isPrimary === false || e.button !== 0) return;
        e.preventDefault();
        const { top, bottom, inset } = this.geometry;
        const value = key === "enterDb" ? this.geometry.entry : this.geometry.exit;
        const lineY = bottom - value / top * (bottom - inset);
        this.drag = { pointerId: e.pointerId, key, top, bottom, inset,
          offset: e.clientY - canvas.getBoundingClientRect().top - lineY };
        handle.setPointerCapture(e.pointerId);
      };
      handle.onpointermove = e => {
        const drag = this.drag;
        if (!drag || drag.pointerId !== e.pointerId || drag.key !== key || !editing.enabled?.()) return;
        e.preventDefault();
        const y = e.clientY - canvas.getBoundingClientRect().top - drag.offset;
        const value = (drag.bottom - y) / (drag.bottom - drag.inset) * drag.top;
        editing.change(key, Math.max(0, Math.min(drag.top, Math.round(value * 2) / 2)));
        this.draw(this.lastNow, this.detector);
      };
      const finish = e => {
        if (this.drag?.pointerId !== e.pointerId || this.drag.key !== key) return;
        this.drag = null;
        if (handle.hasPointerCapture(e.pointerId)) handle.releasePointerCapture(e.pointerId);
      };
      handle.onpointerup = finish;
      handle.onpointercancel = finish;
      handle.onlostpointercapture = finish;
      handle.onkeydown = e => {
        if (!editing.enabled?.() || !this.geometry) return;
        const direction = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
        if (!direction) return;
        e.preventDefault();
        const value = key === "enterDb" ? this.geometry.entry : this.geometry.exit;
        editing.change(key, value + direction * (e.shiftKey ? 5 : 0.5));
        this.draw(this.lastNow, this.detector);
      };
    }
  }
  get hasData() { return this.buckets.length > 0; }
  draw(now, detector) {
    this.lastNow = now;
    this.detector = detector;
    const { canvas, output } = this;
    const width = canvas.clientWidth, height = canvas.clientHeight;
    if (!width || !height) return;
    const ratio = globalThis.devicePixelRatio || 1;
    const pixelWidth = Math.round(width * ratio), pixelHeight = Math.round(height * ratio);
    if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) { canvas.width = pixelWidth; canvas.height = pixelHeight; }
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    const latest = this.buckets.at(-1);
    const end = Math.max(now ?? 0, latest?.lastT ?? 0), start = end - WINDOW_S;
    const data = this.buckets.filter(b => b.lastT >= start);
    const entry = detector?.enterDb ?? 10, exit = detector?.exitDb ?? 5;
    // Freeze the scale while dragging so the line follows the finger steadily.
    const top = this.drag?.top ?? Math.ceil(Math.max(30, entry + 5, ...data.map(b => b.max + 3)) / 10) * 10;
    const left = 32, right = width - 8, bottom = height - 26, inset = 26;
    this.geometry = { top, bottom, inset, entry, exit };
    const x = t => left + (t - start) / WINDOW_S * (right - left);
    const y = v => bottom - Math.max(0, v) / top * (bottom - inset);
    for (const handle of this.editing.handles || []) {
      const isEntry = handle.dataset.key === "enterDb", value = isEntry ? entry : exit;
      handle.style.top = `${y(value)}px`;
      handle.textContent = `${isEntry ? "Entry" : "Exit"} ${value.toFixed(1)}`;
      handle.setAttribute("aria-valuenow", String(value));
      handle.setAttribute("aria-valuemin", String(isEntry ? exit + 0.5 : 0));
      handle.setAttribute("aria-valuemax", String(isEntry ? 80 : entry - 0.5));
      handle.setAttribute("aria-valuetext", `${value} dB above floor`);
    }
    ctx.font = '11px "IBM Plex Sans", sans-serif';
    ctx.fillStyle = "#8a919c";
    ctx.strokeStyle = "#2a2f37";
    ctx.lineWidth = 1;
    for (let db = 0; db <= top; db += 10) {
      ctx.fillText(String(db), 2, y(db) + 4);
      ctx.beginPath(); ctx.moveTo(left, y(db)); ctx.lineTo(right, y(db)); ctx.stroke();
    }
    for (const seconds of [20, 10, 0]) {
      ctx.textAlign = seconds === 20 ? "left" : seconds === 0 ? "right" : "center";
      ctx.fillText(seconds ? `−${seconds}s` : "now", x(end - seconds), height - 4);
    }
    ctx.textAlign = "left";
    ctx.save();
    ctx.beginPath(); ctx.rect(left, 0, right - left, bottom); ctx.clip();
    for (const [value, color] of [[entry, "#d9a33a"], [exit, "#3dbb7c"]]) {
      ctx.strokeStyle = color; ctx.setLineDash([5, 4]);
      ctx.beginPath(); ctx.moveTo(left, y(value)); ctx.lineTo(right, y(value)); ctx.stroke();
    }
    ctx.setLineDash([]);
    ctx.strokeStyle = this.color;
    ctx.globalAlpha = 0.4;
    ctx.beginPath();
    for (const b of data) { ctx.moveTo(x(b.t), y(b.min)); ctx.lineTo(x(b.t), y(b.max)); }
    ctx.stroke(); ctx.globalAlpha = 1;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    data.forEach((b, i) => {
      if (!i || b.t - data[i - 1].lastT > 0.25) ctx.moveTo(x(b.t), y(b.value));
      else ctx.lineTo(x(b.t), y(b.value));
    });
    ctx.stroke();
    ctx.strokeStyle = "#eef0f3"; ctx.lineWidth = 1;
    for (const t of this.passes) {
      ctx.beginPath(); ctx.moveTo(x(t), 5); ctx.lineTo(x(t), bottom); ctx.stroke();
    }
    ctx.restore();
    const fresh = latest && end - latest.lastT < 1;
    output.textContent = fresh ? `${Math.max(0, latest.value).toFixed(1)} dB${detector?.inPass ? " · in gate" : ""}` : latest ? "No recent signal" : "Waiting…";
    canvas.setAttribute("aria-label", `RSSI history: ${output.textContent}. Entry ${entry} dB, exit ${exit} dB above floor.`);
  }
}
