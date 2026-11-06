# Status LED A/B bench test — 6 November 2026

Waveshare C5-Zero, eight distinct Raceband frequencies, gain 30, native USB on Windows.
12.5 minutes on published 1.2.5 and 12.5 minutes on 1.2.6-led.1: each image ran
375 seconds of full-rate scanner capture and 375 seconds of RH API 36 wire polling.
Total measured duration: 1500.124 seconds, excluding flashing/setup.
Baseline SHA-256 matched the published 1.2.5 application:
`ffd45b2f493e1ea6327016d33aba6365a5576b42a6f7f0e2ea3b08c769d01bf2`.
LED image: `43e66451bfe76f5088e59d593cb33d532e2512f9bf17a6114db72d55745a91ee`.

| Measurement | 1.2.5 | 1.2.6-led.1 |
| --- | ---: | ---: |
| Scanner records | 367670 | 367659 |
| Scanner mean cycle | 940.835 us | 942.509 us |
| Scanner p99 cycle | 1188 us | 1190 us |
| Scanner maximum cycle | 1206 us | 1208 us |
| Mean delivered sample interval | 1020.001 us | 1020.000 us |
| Maximum delivered sample interval | 1866 us | 1754 us |
| Scanner sequence gaps / device drops / CRC errors | 0 / 0 / 0 | 0 / 0 / 0 |
| Scanner failed captures / clips | 0 / 0 | 0 / 0 |
| RH valid replies | 92103 | 95953 |
| RH timeouts / checksums / seat mismatches / uptime resets | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| RH sampled loop interval p99 / maximum | 1000 / 1000 us | 1000 / 2000 us |
| RH scan cycles (device counters) | 389265 | 387706 |
| RH mean measured visit work (nodeprof) | 120.2 us | 120.7 us |

Both final device statuses reported zero dump timeouts, incomplete captures,
queue drops, failed node captures and stale-dropped samples. Scanner USB was
drained during warmup to avoid startup backpressure. RH polled all eight seat
indices/frequencies and lap stats about ten times per second, with LED enabled
and the cyan traffic pattern active.

The LED candidate adds about 1.67 us (0.178%) to the scanner mean cycle. RH cycle
count is about 0.4% lower. No material throughput/continuity regression was observed
in this workload; this is not a claim of zero CPU overhead. Lower maximum scanner
gap is an observation, not evidence that LEDs improve timing.

## Limits

No controlled RF source, flying passes, sensitivity measurement, absolute lap-time
comparison, full RH server, S3 UART traffic, power-only cable, host suspend,
power-cycle test, radio-failure injection or physical XIAO LED test was included.
RH loop intervals are millisecond-resolution snapshots, not an every-sample trace.
Host request latency used Windows monotonic time with roughly 16 ms quantization;
its sub-millisecond averages are not a reliable latency comparison. These are two
sequential runs, not a randomized repeated experiment.

Release 1.2.6 isolates LED changes on v1.2.5, excludes unrelated raw diagnostics,
and changes version metadata from the tested development image. Exact release
hashes and validation scope accompany the package in release/SOURCE_MANIFEST.json
and release/QUALIFICATION.json.

[Machine-readable results](validation/status-led-20261106.json).
