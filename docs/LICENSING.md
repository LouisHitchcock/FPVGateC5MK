# Licensing

This explains the licence and the reasoning behind it. It isn't legal advice.
If you plan commercial use, or rely on the reasoning below for a commercial
decision, have it checked by an IP solicitor.

## In short

| You want to... | Allowed? |
|---|---|
| Build it and use it yourself, at home or at a club | Yes, under CC BY-NC-SA 4.0 |
| Change it and share your changes | Yes. Credit the author and share under the same licence. |
| Build it into your own non-commercial timer and share that | Yes, under the same licence |
| Use it at a free event | Yes |
| Sell hardware that runs it, sell the firmware, or use it in a paid product or service | Only with a commercial licence (see below) |

## The non-commercial licence

FPVGate C5MK is licensed under **Creative Commons
Attribution-NonCommercial-ShareAlike 4.0** (CC BY-NC-SA 4.0), the same licence
as FPVGate:

- **Attribution:** credit the author, link to the licence, and say if you
  changed anything.
- **NonCommercial:** no use "primarily intended for or directed towards
  commercial advantage or monetary compensation" (the licence's own words).
- **ShareAlike:** if you share a modified version, it has to be under the same
  licence.

Full terms: <https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode>. A
summary is in [`LICENSE`](../LICENSE).

## Commercial licensing

Commercial use needs a separate licence from the copyright holder, Louis
Hitchcock. For example:

- selling timing hardware that uses this firmware;
- selling the firmware, or modules or kits with it loaded;
- running a paid service that uses it.

Terms are agreed case by case. To ask, email **louishitchcock@gmail.com**.

A commercial licence covers this project's own code. The third-party parts of
a firmware binary keep their own licences (see the last section), and a
commercial licensee still needs to meet those terms. They aren't onerous.

Not sure whether something counts as commercial, such as a club race that
charges entry to cover its costs? Ask.

## Why this isn't covered by the GPL

Other projects have shown that the ESP32-C5's radio can capture raw I/Q and
receive 5.8 GHz FPV signals: esp-sdr, C5VRX and double-ESP-resso. Their code
is GPL, so it's fair to ask whether this firmware has to be GPL too. It
doesn't, for these reasons.

### What the GPL covers

The GPL is a copyright licence. Its conditions apply when you copy, change or
distribute GPL code, or a program based on it (including one that links it
in). Copyright protects the way code is written. It doesn't protect:

- ideas, methods or facts, such as "the C5's PHY can dump raw I/Q samples to
  SRAM" or "bits 0-9 of each sample word are I". (US: 17 U.S.C. 102(b). UK and
  EU: *SAS Institute v World Programming*, decided by the EU Court of Justice
  in 2012 and the UK Court of Appeal in 2013, which held that what a program
  does, and the ideas behind it, are not protected.)
- separate code written independently to do a similar job.

So the question isn't whether this firmware does something similar. It's
whether it contains or is based on their code. It doesn't.

### How it was kept independent

C5MK was built with a two-sided clean-room process:

- **The research side** studied the problem, including reading GPL code, and
  wrote down **facts only**: register addresses, bit positions, call orders,
  timings and measurements. That specification contains no code and no
  pseudo-code from any GPL project, and tags every fact that was first learned
  from GPL code so it could be checked independently.
- **The implementation side** wrote this firmware from that facts-only
  specification, Espressif's Apache-2.0 headers and libraries (including our
  own disassembly of them), ESP-IDF documentation and our own bench
  measurements. It never read GPL source.
- No code moved from the research side to this repository. None of the GPL
  projects is a dependency, and nothing from them is linked into or shipped
  with this firmware.

Most of the facts the firmware relies on came from Espressif's own Apache-2.0
libraries, read independently, and from our own measurements. The few that
were first learned from GPL code are listed in
[`PROVENANCE.md`](PROVENANCE.md), with how each was checked.

### What follows from that

Because the firmware isn't based on GPL code, its author can choose its
licence. That's why it can be non-commercial with commercial licences sold
separately, which a GPL-based project couldn't do.

## Third-party components

All of the source code in this repository is original. A compiled firmware
binary also contains other people's code, under their own licences:

| Component | Licence | What it means |
|---|---|---|
| ESP-IDF, including Espressif's prebuilt Wi-Fi, PHY and RF test libraries (`libphy.a`, `librftest.a`) | Apache-2.0 | Permissive. Keep Espressif's licence and notices when you distribute a binary. |
| FreeRTOS, newlib and other parts of ESP-IDF | MIT and BSD-style | Permissive. Keep the notices. |

The firmware is built on plain ESP-IDF, without the Arduino core, so no LGPL
code is involved.

## Contributions

Contributions are welcome. Because the project is also licensed commercially,
contributors may be asked to sign a short agreement letting their changes go
into commercially licensed versions too. Without one, a contribution can only
be used under CC BY-NC-SA.
