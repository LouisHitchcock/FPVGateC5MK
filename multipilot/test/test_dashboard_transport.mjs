import assert from "node:assert/strict";
import { UsbSerialPort, requestReceiver, knownReceivers, watchReceivers, receiverProblem } from "../web/dashboard-transport.js";

const originalNavigator = Object.getOwnPropertyDescriptor(globalThis, "navigator");
function useNavigator(value) { Object.defineProperty(globalThis, "navigator", {value, configurable:true}); }

// Desktop continues to use the browser's native serial port without wrapping.
const desktopPort = {getInfo:()=>({usbVendorId:0x303a})};
const serial = new EventTarget();
serial.requestPort = async options => { assert.equal(options.filters[0].usbVendorId, 0x303a); return desktopPort; };
serial.getPorts = async () => [desktopPort,{getInfo:()=>({usbVendorId:0x1234})}];
useNavigator({serial});
assert.equal(receiverProblem(), null);
assert.equal(await requestReceiver(), desktopPort);
assert.deepEqual(await knownReceivers(), [desktopPort]);

// Android WebUSB is adapted to the dashboard's existing byte-stream decoder,
// including CDC interface setup, native capture bytes, writes and cancellation.
const calls = [];
const device = {
  vendorId:0x303a, productId:0x1001,
  configuration:{interfaces:[
    {interfaceNumber:0,alternates:[{interfaceClass:2}]},
    {interfaceNumber:1,alternates:[{interfaceClass:10,endpoints:[{type:"bulk",direction:"in",endpointNumber:2},{type:"bulk",direction:"out",endpointNumber:3}]}]},
  ]},
  async open() {calls.push("open");},
  async close() {calls.push("close");},
  async claimInterface(n) {calls.push(`claim ${n}`);},
  async controlTransferOut(options) {calls.push(`control ${options.request}`);},
  async transferIn(endpoint) {assert.equal(endpoint,2);return {status:"ok",data:new DataView(new Uint8Array([0xa5,0x5a,0x43]).buffer)};},
  async transferOut(endpoint, bytes) {assert.equal(endpoint,3);calls.push([...bytes]);},
};
const usb = new EventTarget();
usb.requestDevice = async ({filters}) => {assert.equal(filters[0].productId,0x1001);return device;};
usb.getDevices = async () => [device,{vendorId:0x1234,productId:0x1001}];
useNavigator({usb});
assert.equal(receiverProblem(), null);
const port = await requestReceiver();
assert.ok(port instanceof UsbSerialPort);
assert.equal((await knownReceivers()).length,1);
await port.open({baudRate:115200});
await port.setSignals({dataTerminalReady:false});
assert.deepEqual(calls.slice(0,5),["open","claim 0","claim 1","control 32","control 34"]);
assert.equal(port.getInfo().usbVendorId,0x303a);
const reader = port.readable.getReader(), writer = port.writable.getWriter();
const packet = await reader.read();
assert.equal(packet.done,false);
assert.deepEqual([...packet.value],[0xa5,0x5a,0x43]);
await writer.write(new Uint8Array([115,116,97,116,117,115,10]));
assert.deepEqual(calls.at(-1),[115,116,97,116,117,115,10]);
await reader.cancel();
assert.equal((await reader.read()).done,true);
assert.equal(calls.at(-1),"close");
reader.releaseLock(); writer.releaseLock();

let connected = null, disconnected = null;
watchReceivers(p=>connected=p,p=>disconnected=p);
usb.dispatchEvent(Object.assign(new Event("connect"),{device}));
assert.equal(connected.device,device);
usb.dispatchEvent(Object.assign(new Event("disconnect"),{device}));
assert.equal(disconnected.device,device);
const before = connected;
usb.dispatchEvent(Object.assign(new Event("connect"),{device:{vendorId:1,productId:1}}));
assert.equal(connected,before,"ignore unrelated USB devices");
let cleaned = false;
const broken = new UsbSerialPort({...device,
  claimInterface:async()=>{throw new Error("interface busy");},
  close:async()=>{cleaned=true;},
});
await assert.rejects(broken.open(),/interface busy/);
assert.equal(cleaned,true,"failed USB opens release partially claimed devices");
const originalWindow = Object.getOwnPropertyDescriptor(globalThis,"window");
Object.defineProperty(globalThis,"window",{value:{isSecureContext:true},configurable:true});
useNavigator({userAgent:"iPhone"});
assert.match(receiverProblem(),/iPhones and iPads/);
if (originalNavigator) Object.defineProperty(globalThis,"navigator",originalNavigator); else delete globalThis.navigator;
if (originalWindow) Object.defineProperty(globalThis,"window",originalWindow); else delete globalThis.window;
console.log("Dashboard transport: desktop serial, Android CDC setup, capture bytes, writes, cancellation, reconnection and error cleanup passed");
