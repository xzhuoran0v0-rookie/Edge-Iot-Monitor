# Competition Writeup

Wording for the proposal, report, and presentation. Everything here describes
what the code actually does.

## Project Name

Edge-Intelligence Environment Monitoring System — On-Device Reasoning with
Cloud Reporting

## One-Sentence Summary

An ESP32-S3 that learns what "normal" means in the room it is installed in and
decides on its own, in under a second, whether conditions have changed — with
Huawei Cloud IoTDA and a web dashboard carrying that decision outward, and a
language model used only to explain it.

## Background

Threshold alarms have two well-known failure modes. A fixed threshold cannot
know that one room idles at 34 °C and another at 18 °C, so it either cries wolf
or stays silent. And a system that ships its data to the cloud to be judged
stops working the moment the network does — precisely when a monitoring system
is least able to afford it.

Cloud LLM analysis is the obvious modern answer, and it inherits both problems:
every decision now depends on connectivity, on an API quota, and on a model
returning parseable output.

This project inverts that. The decision runs on the microcontroller. The cloud
receives conclusions rather than producing them.

## Technical Route

An ESP32-S3 reads an SHT30 over I²C. A dedicated FreeRTOS task samples at 1 Hz
and checks fixed safety limits, independent of all network activity. Every 10
seconds the main loop median-filters the reading and passes it through two
reasoning layers:

`AdaptiveBaseline` learns the ambient range of the actual installation —
incremental center and deviation, bounded bands, slow learning rates,
out-of-band samples excluded from learning, state persisted to NVS so a reboot
does not restart the education.

`EdgeReasoner` evaluates a 12-sample sliding window against fixed thresholds and
per-minute rates, producing a state, a severity, a confidence, and a machine-
readable reason code.

The two combine: a hard-limit breach overrides everything; a baseline departure
is reported only when the fixed layer sees nothing. The result drives the OLED
immediately, then goes out over MQTT/MQTTS to Huawei Cloud IoTDA as a property
report, and over HTTP to a local C++ service that stores it in SQLite and serves
a web dashboard.

## System Loop

```text
SHT30
  -> 1 Hz safety task (hard limits)
  -> median filter -> AdaptiveBaseline -> EdgeReasoner
  -> EdgeAssessment {state, severity, confidence, reason}
  -> OLED                          (immediate, no network)
  -> IoTDA property report          (Environment + EdgeReasoning services)
  -> local backend                  (SQLite, IQR, dashboard)
  -> optional LLM narration         (explains; decides nothing)
```

## Innovation Points

1. **The decision is on the chip.** No network appears anywhere in the path from
   sample to verdict. Pulling the Wi-Fi during a demo changes nothing about the
   device's behaviour — which is the fastest way to show the architecture is
   real.

2. **Adaptive, but provably bounded.** The learned band is always clamped inside
   immutable hard limits. Learning can narrow attention; it can never widen the
   safety boundary. Separating the part that adapts from the part that
   guarantees is what makes an adaptive system safe to deploy.

3. **A long anomaly cannot become the new normal.** Samples outside the band are
   not learned from, so a week-long heatwave does not train the device into
   accepting it.

4. **The system says when it does not trust itself.** `UNSTABLE` /
   `ERRATIC_SIGNAL` is checked before every other rule: if the signal is too
   erratic to trust, the device reports that instead of a confident conclusion
   drawn from noise.

5. **The LLM explains rather than decides — by construction.** No code path
   converts model output into an alert or a command, and the prompt says so.
   Freed from being load-bearing, the model stops hedging and stops inflating
   severity.

6. **Narration fires on state change, not on a record count.** A count-based
   trigger re-analyses identical steady-state data forever, so the model can
   only repeat its input back while consuming quota. Steady state produces no
   call at all.

7. **No credential can leak from the device**, because the device never calls a
   model. There is nothing on it to extract.

8. **Zero marginal cost per decision.** 8,640 assessments per device per day,
   none billable.

9. **Degradation is designed, not incidental.** Exponential backoff on the
   backend, independent MQTT retry, stale-sample guards that block reporting
   rather than sending a bad value, and a dashboard that distinguishes "backend
   down" from "device silent".

## Safety and Security

- The decision path contains no network call, no cloud dependency, and no model.
- Hard safety limits are immutable and checked at 1 Hz on a dedicated task.
- Adaptive bands are clamped inside those limits and can never widen them.
- Stale or physically invalid samples block reporting instead of being sent.
- The ESP32-S3 stores no LLM API key; keys live only in server configuration.
- Device commands are allowlisted (`buzzer_on`, `buzzer_off`, `oled:<text>`)
  with a bounded duration, so no arbitrary GPIO control is exposed.
- Ingest enforces a device allowlist; the command API can require a key.
- IoTDA property reports use the official topic and JSON structure.
- The buzzer GPIO is held high-impedance by default, so a command cannot
  energise a circuit that has not passed hardware verification.

## Presentation Wording

```text
This project is an environment monitoring system whose intelligence runs on the
device. An ESP32-S3 with an SHT30 sensor learns the ambient temperature and
humidity range of the room it is installed in, and evaluates each reading
against both that learned baseline and a set of fixed safety limits. The
assessment — a state, a severity, a confidence, and a reason — is produced
entirely on the microcontroller, in deterministic code, with no network call
anywhere in the decision path.

The device reports that assessment to Huawei Cloud IoTDA over MQTT/MQTTS as a
standard property report, and to a local C++ service that stores it in SQLite
and serves a live web dashboard. A language model is available to turn a state
change into a readable explanation, but it is never asked what to conclude: no
code path converts model output into an alert or a command.

Compared with a cloud-decides architecture, this system keeps working when the
network does not, responds in one second rather than one round trip, exposes no
credential on the device, and costs nothing per decision. Compared with a fixed
threshold alarm, it adapts to the room it is actually in — while keeping the
adaptive band mathematically bounded inside safety limits that learning cannot
move.
```

## Honest Limitations

Worth stating before a judge finds them:

- The backend → IoTDA command downlink (`CloudSync`) is a skeleton. The device's
  own MQTT publish to IoTDA works; the server-side forwarding direction is not
  implemented.
- The buzzer is disabled by default pending hardware verification. The OLED is
  the working alert output.
- Wi-Fi, MQTT, and NTP status are shown on the OLED but are not part of the
  ingest payload, so the web dashboard does not display them.
- IoTDA MQTT requires registered device credentials to demonstrate.

## Demo Script

1. Show live readings and the device verdict on the dashboard.
2. Warm the sensor. Within one cycle the OLED and the dashboard both show
   `TEMP RISING`, with a confidence and the time the state began.
3. **Pull the Wi-Fi.** The OLED keeps assessing correctly. This is the argument
   for the whole architecture, and it takes five seconds to make.
4. Reconnect; backoff recovers and reporting resumes.
5. Show `edge_assessments` in SQLite: a state transition timeline, not a wall of
   duplicates.

Without hardware: `python3 scripts/simulate_sensor.py --interval 2` drives the
same path, and `--force-edge-state HARD_LIMIT` demonstrates the safety state.
