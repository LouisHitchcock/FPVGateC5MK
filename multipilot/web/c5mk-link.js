// The C5MK USB link for the browser pages (timer, VTX check): opening the C5's
// USB port, the framing, and decoding its records. The dashboard (index.html)
// has its own copy of the same logic.
//
// Transports:
//   - Web Serial (Chrome, Edge, Opera, Firefox on desktop).
//   - WebUSB, talking CDC-ACM to the C5's USB Serial/JTAG port directly, where
//     there's no Web Serial (Chrome on Android). Not tested on a phone yet.
//
// Frames from the C5 (multipilot/src/frame.h):
//   A5 5A | type | length (u16 LE) | payload | CRC-16/X-25 (u16 LE)
// 'T' is text, 'R' one scan cycle: a 12-byte header and 14 bytes per entry.

export const ESPRESSIF = { vendorId: 0x303A, productId: 0x1001 };
export const FLAG = { FAIL: 1, GAIN: 2, CLIP: 4, FALLBACK: 8 };

// Public 5.8 GHz channel tables, MHz.
export const BANDS = {
  R: [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917],
  A: [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725],
  B: [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866],
  E: [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945],
  F: [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880],
  L: [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621],
};
export const BAND_LABEL = { R: "Raceband", A: "Boscam A", B: "Boscam B", E: "Boscam E", F: "Fatshark", L: "Lowband" };
export const MIN_MHZ = 5180, MAX_MHZ = 5917;   // what the C5 tunes

export function channelName(mhz) {
  for (const b of ["R", "F", "E", "A", "B", "L"]) {   // Raceband first for shared frequencies
    const i = BANDS[b].indexOf(mhz);
    if (i >= 0) return b + (i + 1);
  }
  return String(mhz);
}

export function crc16x25(b, start, end) {
  let c = 0xFFFF;
  for (let i = start; i < end; i++) {
    c ^= b[i];
    for (let k = 0; k < 8; k++) c = (c & 1) ? (c >>> 1) ^ 0x8408 : c >>> 1;
  }
  return (~c) & 0xFFFF;
}

// Which way this browser can reach the C5, or null.
export function transportKind() {
  if ("serial" in navigator) return "serial";
  if ("usb" in navigator) return "usb";
  return null;
}

export function whyNoTransport() {
  if (!window.isSecureContext) return "USB only works on secure pages: open the https:// address.";
  if (/iPhone|iPad|iPod/.test(navigator.userAgent)) return "iPhones and iPads can't connect to USB devices from a browser. Use an Android phone or a computer.";
  return "This browser can't talk to USB devices. Use Chrome on Android, or Chrome, Edge or Firefox on a computer.";
}

// ---- WebUSB CDC-ACM ----------------------------------------------------------
// The C5's USB Serial/JTAG port is a standard CDC-ACM serial port. On Android
// nothing claims it, so WebUSB can drive it directly.
class UsbCdc {
  constructor(device) { this.device = device; }

  static async request() {
    return new UsbCdc(await navigator.usb.requestDevice({ filters: [ESPRESSIF] }));
  }

  static async known() {
    const list = await navigator.usb.getDevices();
    const d = list.find(x => x.vendorId === ESPRESSIF.vendorId && x.productId === ESPRESSIF.productId);
    return d ? new UsbCdc(d) : null;
  }

  async open() {
    const d = this.device;
    await d.open();
    if (!d.configuration) await d.selectConfiguration(1);
    let ctrl = null, data = null;
    for (const itf of d.configuration.interfaces) {
      const a = itf.alternates[0];
      if (a.interfaceClass === 0x02 && ctrl === null) ctrl = itf.interfaceNumber;
      if (a.interfaceClass === 0x0A && data === null) {
        data = itf.interfaceNumber;
        for (const ep of a.endpoints) {
          if (ep.type !== "bulk") continue;
          if (ep.direction === "in") this.epIn = ep.endpointNumber;
          else this.epOut = ep.endpointNumber;
        }
      }
    }
    if (data === null || this.epIn === undefined || this.epOut === undefined) throw new Error("no CDC data interface on this device");
    if (ctrl !== null) await d.claimInterface(ctrl);
    await d.claimInterface(data);
    this.ctrl = ctrl ?? 0;
    // 115200 8N1 (ignored by the C5, but some stacks want it), then DTR and RTS
    // low so the C5 isn't reset.
    const coding = new Uint8Array([0x00, 0xC2, 0x01, 0x00, 0, 0, 8]);
    await d.controlTransferOut({ requestType: "class", recipient: "interface", request: 0x20, value: 0, index: this.ctrl }, coding).catch(() => {});
    await d.controlTransferOut({ requestType: "class", recipient: "interface", request: 0x22, value: 0, index: this.ctrl }).catch(() => {});
  }

  async read() {
    const r = await this.device.transferIn(this.epIn, 16384);
    if (r.status !== "ok" || !r.data) return new Uint8Array(0);
    return new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
  }

  async write(bytes) { await this.device.transferOut(this.epOut, bytes); }

  async close() { try { await this.device.close(); } catch (e) {} }
}

// ---- the link ------------------------------------------------------------------
// Events (addEventListener):
//   "state"  detail: { connected, text }
//   "record" detail: { seq, cycleUs, entries: [{ t, raw, filt, mhz, gain, clips, flags }] }
//            t is the C5's clock in seconds (unwrapped), raw/filt in dB (raw NaN
//            if the capture failed)
//   "text"   detail: string from the C5
export class C5Link extends EventTarget {
  constructor() {
    super();
    this.kind = transportKind();
    this.port = null;        // SerialPort or UsbCdc
    this.reader = null;
    this.writer = null;
    this.connected = false;
    this.userDisconnected = false;
    this.rx = new Uint8Array(0);
    this.stats = { lost: 0, crc: 0, records: 0 };
    this._lastSeq = -1;
    this._dropsRaw = 0;
    this._settleUntil = 0;
    this._tOffset = 0;
    this._tPrevUs = -1;
  }

  _state(text) {
    this.dispatchEvent(new CustomEvent("state", { detail: { connected: this.connected, text } }));
  }

  // Asks the user to pick the C5. Returns true once connected.
  async connect() {
    this.userDisconnected = false;
    let p;
    try {
      if (this.kind === "serial") p = await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF.vendorId, usbProductId: ESPRESSIF.productId }] });
      else if (this.kind === "usb") p = await UsbCdc.request();
      else return false;
    } catch (e) {
      return false;   // picker closed
    }
    return this._open(p, false);
  }

  // A C5 this browser has already been allowed to use: connect without asking.
  async autoConnect() {
    if (this.connected || this.userDisconnected) return false;
    try {
      if (this.kind === "serial") {
        const ports = await navigator.serial.getPorts();
        const p = ports.find(q => q.getInfo().usbVendorId === ESPRESSIF.vendorId);
        if (p) return this._open(p, true);
      } else if (this.kind === "usb") {
        const d = await UsbCdc.known();
        if (d) return this._open(d, true);
      }
    } catch (e) {}
    return false;
  }

  // Reconnect when the C5 is plugged back in.
  watch() {
    const api = this.kind === "serial" ? navigator.serial : this.kind === "usb" ? navigator.usb : null;
    if (!api) return;
    api.addEventListener("connect", () => { if (!this.connected) setTimeout(() => this.autoConnect(), 300); });
    api.addEventListener("disconnect", () => { if (this.connected) this._lost(); });
  }

  async _open(p, quiet) {
    if (this.connected) return true;
    this._state("Connecting…");
    try {
      if (this.kind === "serial") {
        await p.open({ baudRate: 115200, bufferSize: 1 << 16 });
        await p.setSignals({ dataTerminalReady: false, requestToSend: false });
      } else {
        await p.open();
      }
    } catch (e) {
      this._state(quiet ? "Not connected" :
        /open|in use|access|claim/i.test(e.message)
          ? "The receiver is in use by another program or tab. Close it and try again."
          : `Couldn't open the receiver: ${e.message}`);
      return false;
    }
    this.port = p;
    this.connected = true;
    this.rx = new Uint8Array(0);
    Object.assign(this.stats, { lost: 0, crc: 0, records: 0 });
    this._lastSeq = -1;
    this._dropsRaw = 0;
    // Bytes left over from before the open (or a reset by it) aren't counted.
    this._settleUntil = performance.now() + 750;
    if (this.kind === "serial") this.writer = p.writable.getWriter();
    this._state("Connected");
    this._readLoop();
    // The C5 says nothing until the host does (C5MK 1.2.2 and later).
    await this.send("status");
    return true;
  }

  async _readLoop() {
    try {
      if (this.kind === "serial") {
        this.reader = this.port.readable.getReader();
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value) this._feed(value);
        }
      } else {
        while (this.connected) {
          const chunk = await this.port.read();
          if (chunk.length) this._feed(chunk);
        }
      }
    } catch (e) {
      // closed or unplugged
    } finally {
      if (this.reader) { try { this.reader.releaseLock(); } catch (e) {} this.reader = null; }
    }
    if (this.connected) this._lost();
  }

  async _lost() {
    await this._close();
    this._state("Disconnected. Plug the receiver back in to reconnect.");
  }

  async disconnect() {
    this.userDisconnected = true;
    await this._close();
    this._state("Not connected");
  }

  async _close() {
    const p = this.port;
    this.connected = false;
    this.port = null;
    try { if (this.reader) await this.reader.cancel(); } catch (e) {}
    try { if (this.writer) { this.writer.releaseLock(); this.writer = null; } } catch (e) {}
    try { if (p) await p.close(); } catch (e) {}
  }

  async send(line) {
    if (!this.connected) return;
    const bytes = new TextEncoder().encode(line.trim() + "\n");
    try {
      if (this.kind === "serial") await this.writer.write(bytes);
      else await this.port.write(bytes);
    } catch (e) {}
  }

  // Up to 8 frequencies, MHz.
  setChannels(list) { return this.send("pilots " + list.join(",")); }

  _settling() { return performance.now() < this._settleUntil; }

  _feed(chunk) {
    const b = new Uint8Array(this.rx.length + chunk.length);
    b.set(this.rx);
    b.set(chunk, this.rx.length);
    let i = 0;
    for (;;) {
      while (i + 1 < b.length && !(b[i] === 0xA5 && b[i + 1] === 0x5A)) i++;
      if (i + 5 > b.length) break;
      const len = b[i + 3] | (b[i + 4] << 8);
      if (len > 4096) { i += 2; continue; }
      if (i + 7 + len > b.length) break;
      const crc = b[i + 5 + len] | (b[i + 6 + len] << 8);
      if (crc16x25(b, i + 2, i + 5 + len) !== crc) { if (!this._settling()) this.stats.crc++; i += 2; continue; }
      this._frame(String.fromCharCode(b[i + 2]), b.slice(i + 5, i + 5 + len));
      i += 7 + len;
    }
    this.rx = b.slice(Math.min(i, b.length));
  }

  _chipSeconds(us) {
    if (this._tPrevUs >= 0 && us < this._tPrevUs && this._tPrevUs - us > 2 ** 31) this._tOffset += 2 ** 32;
    this._tPrevUs = us;
    return (us + this._tOffset) / 1e6;
  }

  _frame(type, p) {
    if (type === "T") {
      this.dispatchEvent(new CustomEvent("text", { detail: new TextDecoder().decode(p) }));
      return;
    }
    if (type !== "R" || p.length < 12) return;
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    const seq = dv.getUint16(0, true), count = dv.getUint8(2), drops = dv.getUint32(4, true);
    // A drop counter that falls means the C5 restarted: start counting again.
    if (drops < this._dropsRaw || this._settling()) this._lastSeq = -1;
    this._dropsRaw = drops;
    if (this._lastSeq >= 0) {
      const run = ((seq - this._lastSeq) & 0xFFFF) - 1;
      if (run > 0) this.stats.lost += run;
    }
    this._lastSeq = seq;
    this.stats.records++;
    const entries = [];
    for (let k = 0; k < count && 12 + 14 * (k + 1) <= p.length; k++) {
      const o = 12 + 14 * k, flags = dv.getUint8(o + 12);
      entries.push({
        t: this._chipSeconds(dv.getUint32(o, true)),
        raw: flags & FLAG.FAIL ? NaN : dv.getInt16(o + 4, true) / 100,
        filt: dv.getInt16(o + 6, true) / 100,
        mhz: dv.getUint16(o + 8, true),
        gain: dv.getUint8(o + 10),
        clips: dv.getUint8(o + 11),
        flags,
      });
    }
    this.dispatchEvent(new CustomEvent("record", { detail: { seq, cycleUs: dv.getUint32(8, true), entries } }));
  }
}
