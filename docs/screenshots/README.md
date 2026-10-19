# Browser screenshots

These are screenshots of the actual browser UI. `dashboard-live.png` is the
receiver screenshot supplied by Louis on 19 October 2026, showing firmware
`1.2.3-dev+xiaoc5`. It is the main dashboard image in the repository README.
Keep this supplied capture unchanged; the capture script does not replace it.

All other images use simulated receiver readings and passes; those images
do not demonstrate RF performance or physical USB compatibility.

- `dashboard-live.png`: supplied receiver capture, eight channels, active R2
  signal, peak hold, one waterfall and advanced tools open.
- `connect.png`: dashboard connection and setup information.
- `dashboard.png`: desktop dashboard, eight channels and one large waterfall.
- `dashboard-mobile.png`: the same dashboard at a 390 px phone viewport.
- `timer-mobile.png`: lap timer setup with its RSSI graph closed by default.
- `timer-rssi-mobile.png`: the expanded RSSI graph, custom entry/exit fields
  and draggable touch handles.
- `timer-desktop.png`: desktop setup, with four pilots beside session settings.
- `timer-rssi-desktop.png`: full-page desktop tuning with graphs in two columns.
- `timer-race-desktop.png`: four live pilot clocks in a two-column desktop layout.
- `timer-results-desktop.png`: desktop lap results with restart and CSV controls.

To regenerate from the repository root, with Node.js 24 and Chrome installed:

```text
node multipilot/tools/capture_web.mjs
```

On Linux, or for another Chrome/Chromium installation, supply its executable:

```text
node multipilot/tools/capture_web.mjs /usr/bin/chromium
```

The script starts a temporary localhost server and a separate headless browser
profile. It serves the real web files, supplies simulated receiver data only
to that local session, and captures PNGs here. Nothing is published and no real
receiver is connected. Phone captures use device emulation and verify a 390 px
layout without horizontal overflow. The timer captures also check that the
graph starts closed, help opens independently, and tuning controls appear when
the graph is expanded. Extra layout checks cover 320 px phones, 768 px tablets,
1024 px laptops and 1920 px displays without generating additional PNGs.
