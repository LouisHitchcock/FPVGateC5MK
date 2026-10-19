// Give the dashboard the same SerialPort-shaped API on desktop and Android.
// Its existing frame decoder also handles raw captures, so keep byte transport
// separate from C5Link's higher-level RSSI event decoder.
import { UsbCdc, ESPRESSIF, whyNoTransport } from "./c5mk-link.js";

export class UsbSerialPort {
  constructor(device) {
    this.device = device;
    this.cdc = new UsbCdc(device);
    this.writable = { getWriter: () => ({ write: bytes => this.cdc.write(bytes), releaseLock() {} }) };
    this.readable = { getReader: () => {
      let cancelled = false;
      return {
        read: async () => {
          if (cancelled) return { done: true };
          const value = await this.cdc.read();
          return { value, done: cancelled };
        },
        cancel: async () => { cancelled = true; await this.close(); },
        releaseLock() {},
      };
    } };
  }
  async open() {
    try { await this.cdc.open(); }
    catch (error) { await this.cdc.close(); throw error; }
  }
  async setSignals() {} // CDC open already sets DTR/RTS low.
  close() { return this.cdc.close(); }
  getInfo() { return { usbVendorId: this.device.vendorId, usbProductId: this.device.productId }; }
}

export function receiverProblem() {
  return !("serial" in navigator) && !("usb" in navigator) ? whyNoTransport() : null;
}

export async function requestReceiver() {
  if ("serial" in navigator) return navigator.serial.requestPort({filters:[{usbVendorId:ESPRESSIF.vendorId,usbProductId:ESPRESSIF.productId}]});
  return new UsbSerialPort(await navigator.usb.requestDevice({filters:[ESPRESSIF]}));
}

export async function knownReceivers() {
  if ("serial" in navigator) return (await navigator.serial.getPorts()).filter(p => p.getInfo().usbVendorId === ESPRESSIF.vendorId);
  if (!("usb" in navigator)) return [];
  return (await navigator.usb.getDevices())
    .filter(d => d.vendorId === ESPRESSIF.vendorId && d.productId === ESPRESSIF.productId)
    .map(d => new UsbSerialPort(d));
}

export function watchReceivers(onConnect, onDisconnect) {
  if ("serial" in navigator) {
    navigator.serial.addEventListener("connect", e => onConnect(e.target));
    navigator.serial.addEventListener("disconnect", e => onDisconnect(e.target));
  } else if ("usb" in navigator) {
    navigator.usb.addEventListener("connect", e => {
      if (e.device.vendorId === ESPRESSIF.vendorId && e.device.productId === ESPRESSIF.productId) onConnect(new UsbSerialPort(e.device));
    });
    navigator.usb.addEventListener("disconnect", e => onDisconnect({device:e.device}));
  }
}
