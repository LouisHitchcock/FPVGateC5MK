// Generate README screenshots from the real pages with deterministic demo data.
// No receiver is needed and no hosted site is modified.
// Run: node multipilot/tools/capture_web.mjs [path/to/chrome]
import http from "node:http";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

const root = fileURLToPath(new URL("../../", import.meta.url));
const web = path.join(root, "multipilot/web");
const output = path.join(root, "docs/screenshots");
const browser = process.argv[2] || (process.platform === "win32" ? "C:/Program Files/Google/Chrome/Application/chrome.exe" : "chromium");
const profile = await fs.mkdtemp(path.join(os.tmpdir(), "c5mk-screenshots-"));
await fs.mkdir(output, { recursive:true });

const simulatedSignal = `function demoLevel(t,i) {
  const peaks = [3.2+i*.16,10.6+i*.18,19.3+i*.18];
  return 4+i*.35+Math.max(0,...peaks.map(c=>27-10*Math.log10(1+((t-c)*12)**2)));
}`;
const finish = `
  await document.fonts.ready;
  await new Promise(r=>setTimeout(r,100));
  if (document.documentElement.scrollWidth > innerWidth) throw new Error('Horizontal page overflow');
  const marker=document.createElement('meta'); marker.name='c5mk-capture'; marker.content='PASS'; document.head.appendChild(marker);
  document.querySelector('header .title small, header .brand .sub')?.replaceChildren('Demo · simulated RSSI');
} catch(error) {
  const marker=document.createElement('meta'); marker.name='c5mk-capture'; marker.content='FAIL '+error.message; document.head.appendChild(marker);
  console.error(error);
}})();`;
const dashboardFixture = `${simulatedSignal}
(async()=>{try {
  for(let attempts=0;!spectrumView && attempts<100;attempts++) await new Promise(r=>setTimeout(r,10));
  if(!spectrumView) throw new Error('Spectrum did not mount');
  setAdvanced(false);
  port={close:async()=>{}};
  setConnected(true);
  const freqs=[5658,5695,5732,5769,5806,5843,5880,5917];
  for(let n=0;n<=4000;n++) {
    const t=n/200;
    const dv=new DataView(new ArrayBuffer(12+14*8));
    dv.setUint16(0,n,true); dv.setUint8(2,8); dv.setUint32(8,1000,true);
    freqs.forEach((mhz,i)=>{ const o=12+14*i, level=Math.round(demoLevel(t,i)*100);
      dv.setUint32(o,Math.round((100+t)*1e6),true); dv.setInt16(o+4,level,true); dv.setInt16(o+6,level,true); dv.setUint16(o+8,mhz,true); dv.setUint8(o+10,30);
    });
    onRecord(dv);
  }
  spectrumView.updateLive(freqs.map((mhz,i)=>({mhz,filt:demoLevel(20,i)})));
  document.querySelector('[data-sp="hold"]').onchange();
  renderCards(); renderWaterfall();
  if(document.querySelectorAll('#wf').length!==1 || document.querySelectorAll('.sp-waterfall').length) throw new Error('Duplicate waterfall');
${finish}`;
const timerFixture = (open, desktop = false, screen = "setup", count = desktop ? 4 : 1) => `${simulatedSignal}
(async()=>{try {
  cfg.pilots=Array.from({length:${count}},(_,i)=>({name:${desktop} ? ['Alex','Sam','Jordan','Casey'][i] : 'Pilot '+(i+1),mhz:[5658,5732,5843,5917][i],enterDb:10,exitDb:5})); cfg.mode='practice';cfg.sens='medium';cfg.voice=false;cfg.customThresholds=${open};
  link.send=async()=>{};link.connected=true;link._state('Connected');
  const sample=(n)=>{const t=n/200;link.dispatchEvent(new CustomEvent('record',{detail:{entries:cfg.pilots.map((p,i)=>({t,mhz:p.mhz,filt:demoLevel(t%20,i)}))}}));};
  for(let n=0;n<=4000;n++) sample(n);
  if(!$('signalBody').hidden || $('btnToggleGraph').getAttribute('aria-expanded')!=='false') throw new Error('Graph is not collapsed by default');
  $('btnSignalHelp').click();
  if($('signalHelp').hidden || $('signalBody').hidden!==true) throw new Error('Help cannot open independently');
  $('btnSignalHelp').click();
  ${open ? `$('btnToggleGraph').click();$('advanced').open=true;graphs[0].draw(nowChip(),detectors[0]);
  if($('signalBody').hidden) throw new Error('Graph did not open');
  const handle=document.querySelector('.signal-handle[data-key="enterDb"]');
  const rect=handle.getBoundingClientRect();
  if(rect.height<44 || rect.width<44) throw new Error('Drag handle too small');
  if(!document.querySelector('.signal-controls input').value) throw new Error('Threshold field empty');` : ""}
  ${screen !== "setup" ? `await start();${screen === "waiting" ? "" : "for(let n=4001;n<=12000;n++) sample(n);"}` : ""}
  ${screen === "results" ? `stop();if(document.querySelectorAll('.pilot-result').length!==cfg.pilots.length) throw new Error('Missing pilot results');` : ""}
  await new Promise(r=>setTimeout(r,100));
  if(innerWidth>=960) {
    if(document.querySelector('main').getBoundingClientRect().width<900) throw new Error('Desktop timer is still phone width');
    ${screen === "setup" ? `if($('setupOptions').getBoundingClientRect().left<=$('setupPilots').getBoundingClientRect().left) throw new Error('Setup columns did not expand');` : ""}
    ${screen === "run" ? `const boxes=[...document.querySelectorAll('.lane')].map(el=>el.getBoundingClientRect());
    if(boxes.length>1 && Math.abs(boxes[0].top-boxes[1].top)>1) throw new Error('Race lanes did not expand into columns');` : ""}
  }
${finish}`;
const vtxFixture = `(async()=>{try {
  link.send=async()=>{};link.connected=true;link._state('Connected');
${finish}`;

const server = http.createServer(async(req,res)=>{
  try {
    const url=new URL(req.url,"http://localhost");
    if(url.pathname==="/dashboard-transport.js") {
      res.setHeader("Content-Type","text/javascript");
      res.end("export const receiverProblem=()=>null;export const knownReceivers=async()=>[];export function watchReceivers(){};");
      return;
    }
    const relative=url.pathname.endsWith("/") ? url.pathname+"index.html" : url.pathname;
    const filename=path.resolve(web,"."+relative);
    if(!filename.startsWith(web+path.sep)) {res.writeHead(403);res.end();return;}
    let source=await fs.readFile(filename);
    if(filename.endsWith(".html")) {
      const fixture=url.searchParams.get("shot");
      if(fixture) {
        const desktop=fixture.startsWith("desktop");
        const screen=fixture.endsWith("race") ? "run" : fixture.endsWith("results") ? "results" : fixture.endsWith("waiting") ? "waiting" : "setup";
        source=source.toString().replace("</script>",(fixture==="dashboard" ? dashboardFixture : fixture==="vtx" ? vtxFixture : timerFixture(fixture.endsWith("tuning"),desktop,screen,desktop||screen!=="setup"?4:1))+"\n</script>");
      }
    }
    const types={".js":"text/javascript",".css":"text/css",".svg":"image/svg+xml",".png":"image/png",".woff2":"font/woff2",".html":"text/html"};
    res.setHeader("Content-Type",types[path.extname(filename)]||"application/octet-stream");
    res.end(source);
  } catch(error) {res.writeHead(404);res.end(error.message);}
});
await new Promise(resolve=>server.listen(0,"127.0.0.1",resolve));
const origin=`http://127.0.0.1:${server.address().port}`;
const captures=[
  {name:"connect",width:1280,height:1100,url:"/"},
  {name:"dashboard",width:1440,height:1350,url:"/?shot=dashboard"},
  {name:"timer-desktop",width:1440,height:1100,url:"/timer/?shot=desktop-timer"},
  {name:"timer-rssi-desktop",width:1440,height:1450,url:"/timer/?shot=desktop-tuning",fullPage:true},
  {name:"timer-race-desktop",width:1440,height:1000,url:"/timer/?shot=desktop-race"},
  {name:"timer-results-desktop",width:1440,height:1100,url:"/timer/?shot=desktop-results"},
  {name:"dashboard-mobile",width:390,height:1250,url:"/?shot=dashboard"},
  {name:"timer-mobile",width:390,height:1100,url:"/timer/?shot=timer"},
  {name:"timer-rssi-mobile",width:390,height:1450,url:"/timer/?shot=tuning"},
  {name:"timer-phone-race-check",width:320,height:900,url:"/timer/?shot=mobile-race",checkOnly:true},
  {name:"timer-phone-waiting-check",width:320,height:900,url:"/timer/?shot=mobile-waiting",checkOnly:true},
  {name:"timer-tablet-race-check",width:768,height:1024,url:"/timer/?shot=tablet-race",checkOnly:true},
  {name:"timer-laptop-race-check",width:1024,height:900,url:"/timer/?shot=desktop-race",checkOnly:true},
  {name:"timer-laptop-results-check",width:1024,height:900,url:"/timer/?shot=desktop-results",checkOnly:true},
  {name:"timer-laptop-setup-check",width:1024,height:900,url:"/timer/?shot=desktop-timer",checkOnly:true},
  {name:"timer-wide-race-check",width:1920,height:1080,url:"/timer/?shot=desktop-race",checkOnly:true},
  {name:"vtx-phone-brand-check",width:390,height:900,url:"/vtx/?shot=vtx",checkOnly:true},
  {name:"vtx-desktop-brand-check",width:1280,height:900,url:"/vtx/?shot=vtx",checkOnly:true},
];
try {
  // Chrome's window-size flag has a minimum desktop viewport. Explicit device
  // emulation is essential: otherwise a 390px PNG can crop a 500px layout.
  const child=spawn(browser,["--headless","--disable-gpu","--no-first-run","--no-default-browser-check","--hide-scrollbars",`--user-data-dir=${profile}`,"--remote-debugging-port=0","about:blank"],{windowsHide:true});
  let launchError=null; child.on("error",error=>launchError=error);
  child.stderr.resume(); child.stdout.resume();
  let socket;
  try {
    let debugPort;
    for(let attempt=0;attempt<100;attempt++) {
      if(launchError) throw launchError;
      try {debugPort=(await fs.readFile(path.join(profile,"DevToolsActivePort"),"utf8")).split("\n")[0];break;}
      catch {await new Promise(r=>setTimeout(r,50));}
    }
    if(!debugPort) throw new Error("Chrome debug endpoint did not start");
    const pages=await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
    const page=pages.find(p=>p.type==="page");
    socket=new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.addEventListener("open",resolve,{once:true});socket.addEventListener("error",reject,{once:true});});
    let next=0; const pending=new Map();
    socket.addEventListener("message",event=>{
      const message=JSON.parse(event.data), callback=pending.get(message.id);
      if(callback) {pending.delete(message.id);message.error?callback.reject(new Error(message.error.message)):callback.resolve(message.result);}
    });
    const call=(method,params={})=>new Promise((resolve,reject)=>{const id=++next;pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));});
    await call("Page.enable");
    for(const shot of captures) {
      await call("Emulation.setDeviceMetricsOverride",{width:shot.width,height:shot.height,deviceScaleFactor:1,mobile:shot.width<500});
      await call("Page.navigate",{url:origin+shot.url});
      let checked=false;
      for(let attempt=0;attempt<150;attempt++) {
        await new Promise(r=>setTimeout(r,50));
        const state=await call("Runtime.evaluate",{expression:`JSON.stringify({url:location.href,ready:document.readyState,marker:document.querySelector('meta[name="c5mk-capture"]')?.content,width:innerWidth,scroll:document.documentElement.scrollWidth})`,returnByValue:true});
        const check=JSON.parse(state.result.value);
        if(check.url!==origin+shot.url) continue;
        if(check.marker?.startsWith("FAIL")) throw new Error(`${shot.name}: ${check.marker}`);
        if(check.ready!=="complete" || (shot.url.includes("shot=") && check.marker!=="PASS")) continue;
        if(check.width!==shot.width || check.scroll>shot.width) throw new Error(`${shot.name}: viewport ${check.width}, content ${check.scroll}, expected ${shot.width}`);
        checked=true;break;
      }
      if(!checked) throw new Error(`${shot.name}: page did not finish loading`);
      const brand=await call("Runtime.evaluate",{expression:`document.fonts.ready.then(()=>JSON.stringify({font:document.fonts.check('16px "IBM Plex Sans"'),background:getComputedStyle(document.body).backgroundColor,overflow:document.documentElement.scrollWidth>innerWidth,gradient:[...document.querySelectorAll('body *')].some(el=>getComputedStyle(el).backgroundImage.includes('gradient(')),laneBorders:[...document.querySelectorAll('.lane')].every(el=>getComputedStyle(el).borderLeftWidth==='1px')}))`,awaitPromise:true,returnByValue:true});
      const branding=JSON.parse(brand.result.value);
      if(!branding.font || branding.background!=="rgb(20, 22, 26)" || branding.overflow || branding.gradient || !branding.laneBorders) throw new Error(`${shot.name}: brand check failed ${JSON.stringify(branding)}`);
      if(shot.url.startsWith('/vtx/')) {
        const button=await call("Runtime.evaluate",{expression:"JSON.stringify({fill:getComputedStyle(document.querySelector('.vx-primary')).backgroundColor,radius:getComputedStyle(document.querySelector('.vx-primary')).borderRadius})",returnByValue:true});
        const style=JSON.parse(button.result.value);
        if(style.fill!=="rgb(31, 111, 204)"||style.radius!=="5px") throw new Error(`${shot.name}: VTX branding mismatch ${button.result.value}`);
      }
      if(!shot.checkOnly) {
        await call("Runtime.evaluate",{expression:"window.scrollTo(0,0);new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))",awaitPromise:true});
        const metrics=shot.fullPage ? await call("Page.getLayoutMetrics") : null;
        const screenshot=await call("Page.captureScreenshot",{format:"png",captureBeyondViewport:!!shot.fullPage,
          ...(metrics?{clip:{x:0,y:0,width:shot.width,height:Math.ceil(metrics.cssContentSize.height),scale:1}}:{}),
        });
        await fs.writeFile(path.join(output,shot.name+".png"),Buffer.from(screenshot.data,"base64"));
      }
      console.log(`${shot.name}${shot.checkOnly?'':'.png'} · ${shot.width} × ${shot.height} · verified`);
    }
  } finally {
    socket?.close();child.kill();
  }
} finally {
  server.closeAllConnections();
  await new Promise(resolve=>server.close(resolve));
}
