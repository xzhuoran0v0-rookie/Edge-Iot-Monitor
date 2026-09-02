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
and checks fixed safety limits and alarm thresholds, independent of all network
activity. Every 2 seconds the main loop median-filters the reading and passes it
through two reasoning layers:

`AdaptiveBaseline` learns the ambient range of the actual installation —
incremental center and deviation, bounded bands, slow learning rates,
out-of-band samples excluded from learning, state persisted to NVS so a reboot
does not restart the education.

`EdgeReasoner` evaluates a 12-sample sliding window against fixed thresholds and
per-minute rates, producing a state, a severity, a confidence, and a machine-
readable reason code.

The two combine: a hard-limit breach overrides everything; a baseline departure
is reported only when the fixed layer sees nothing. Crossing a local alarm
threshold sounds the buzzer and writes the cause to the OLED within a second,
with nothing in between.

The same verdict then leaves the device twice: over MQTT/MQTTS to Huawei Cloud
IoTDA as a standard property report, and over HTTP to a local C++ service that
stores it in SQLite. Both are recording paths. Neither is consulted to produce
the decision, and neither can prevent the alarm.

## System Loop

```text
SHT30
  -> 1 Hz safety task (hard limits + local alarm thresholds)
  -> buzzer + OLED reason           (immediate, no network, ~1 s)
  -> median filter -> AdaptiveBaseline -> EdgeReasoner
  -> EdgeAssessment {state, severity, confidence, reason}
  -> OLED                           (immediate, no network)
  -> IoTDA property report          (Environment + EdgeReasoning services)
  -> local backend                  (SQLite, IQR)
```

Everything below the buzzer line is recording. The device has already decided
and already alarmed by the time any of it runs.

## Scope

What is delivered is the **device**: sensing, learned baseline, deterministic
assessment, and a local alarm that sounds within a second without a network.
That path is complete and verified on hardware.

Around it sit two recording paths — Huawei Cloud IoTDA over MQTTS, and a local
C++ service with SQLite — both verified. A local web dashboard and an optional
LLM narration layer exist and work, but they are development and verification
tools rather than deliverables: the system monitors and alarms correctly with
both switched off. Their production form is described under Future Work.

## Innovation Points

1. **The decision is on the chip.** No network appears anywhere in the path from
   sample to verdict. Pulling the Wi-Fi during a demo changes nothing about the
   device's behaviour — which is the fastest way to show the architecture is
   real.

2. **Adaptive, but provably bounded.** The learned band is always clamped inside
   immutable hard limits. Learning can narrow attention; it can never widen the
   safety boundary. Separating the part that adapts from the part that
   guarantees is what makes an adaptive system safe to deploy.

3. **What the device learns can never change what it alarms on.** Alarm
   thresholds and hard limits are fixed, human-set values that are never
   learned, and the adaptive band is clamped inside them. Relearning a warmer
   room changes which readings are called a *pattern shift*; it can never
   change which ones sound the buzzer. That separation is what makes it safe to
   let the baseline move at all.

4. **A passing anomaly and a relocation are told apart by duration.** Samples
   outside the band are never learned from, so a brief excursion cannot drag
   the baseline along. But a departure sustained for an hour means the learned
   band describes somewhere else, and the device recalibrates. Without that, a
   relocated device is stuck reporting `BASELINE_SHIFT` forever — out-of-band
   samples are not learned, so the band can never move again.

5. **The system says when it does not trust itself.** `UNSTABLE` /
   `ERRATIC_SIGNAL` is checked before every other rule: if the signal is too
   erratic to trust, the device reports that instead of a confident conclusion
   drawn from noise.

6. **The LLM explains rather than decides — by construction.** No code path
   converts model output into an alert or a command, and the prompt says so.
   Freed from being load-bearing, the model stops hedging and stops inflating
   severity.

7. **Narration fires on state change, not on a record count.** A count-based
   trigger re-analyses identical steady-state data forever, so the model can
   only repeat its input back while consuming quota. Steady state produces no
   call at all.

8. **No credential can leak from the device**, because the device never calls a
   model. There is nothing on it to extract.

9. **Zero marginal cost per decision.** 43,200 assessments per device per day
   at the 2 s sensing interval, all made on the chip, none billable. Cloud
   traffic is throttled separately to 1,440 messages/day — 14% of a 10,000/day
   free tier, which is what lets one allowance cover about six devices.

10. **The alarm fires before the network exists.** Threshold evaluation lives in
    the 1 Hz safety task, not the main loop, so a device powered on into an
    already-unsafe room sounds within a second instead of waiting out Wi-Fi, NTP
    and MQTT connection. While it is sounding, no remote command can switch it
    off. Both properties were verified on hardware.

11. **The alert names its cause.** The OLED shows which quantity, its value, and
    the limit it crossed — `ALARM  TEMP 31.2C LIMIT 30.0C` — rather than a bare
    label a person still has to interpret.

12. **Degradation is designed, not incidental.** Exponential backoff on the
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
- The buzzer GPIO is held high-impedance whenever `ENABLE_BUZZER` is 0, so a
  command cannot energise a circuit that has not passed hardware verification.
  The shipped example config keeps it that way; this build enables it only
  because the module and its active level were tested.
- A local alarm cannot be silenced by a remote command.

## Presentation Wording

```text
This project is an environment monitoring system whose intelligence runs on the
device. An ESP32-S3 with an SHT30 sensor learns the ambient temperature and
humidity range of the room it is installed in, and evaluates each reading
against both that learned baseline and a set of fixed safety limits. The
assessment — a state, a severity, a confidence, and a reason — is produced
entirely on the microcontroller, in deterministic code, with no network call
anywhere in the decision path.

Crossing a local threshold sounds a buzzer and writes the cause to the OLED
within a second. The device then reports the same assessment to Huawei Cloud
IoTDA over MQTT/MQTTS as a standard property report, and to a local C++ service
that stores it in SQLite. Both are recording paths: neither is consulted to
reach the decision, and neither can prevent the alarm. A language model is
available to turn a state change into a readable explanation, but it is never
asked what to conclude.

Compared with a cloud-decides architecture, this system keeps working when the
network does not, responds in one second rather than one round trip, exposes no
credential on the device, and costs nothing per decision. Compared with a fixed
threshold alarm, it adapts to the room it is actually in — while keeping the
adaptive band mathematically bounded inside safety limits that learning cannot
move.
```

## Honest Limitations

Worth stating before a judge finds them:

- Commands are issued from the local backend's own HTTP queue. Issuing them
  through IoTDA's application-side API (AK/SK) is not built; the device accepts
  such commands, but nothing sends them.
- Visualisation is local only. The dashboard runs on a machine on the same
  subnet as the device, which is fine for development and wrong for deployment.
- Wi-Fi, MQTT, and NTP status are shown on the OLED but are not part of the
  ingest payload, so nothing downstream can display them.
- `config.example.h` ships with the buzzer disabled, so anyone reproducing the
  build must verify their module's active level before enabling it.
- One device. The data model is keyed by `device_id` throughout, but there is no
  grouping, per-device configuration, or alarm escalation.

## Future Work

The device side is finished. What remains is the system around it, and each item
below is a consequence of a limitation stated above rather than a wish list.

**Cloud visualisation.** Today's dashboard needs a laptop on the device's
subnet. Forwarding IoTDA data into a hosted view removes that dependency and
makes the data reachable from anywhere — which is what turns a demonstrator into
something deployable. The device side needs no change: it already publishes its
verdict as an `EdgeReasoning` service property.

**Cloud command downlink.** The device already accepts and acknowledges
allowlisted commands over both transports, and the uplink to IoTDA is verified.
What is missing is a server issuing commands through IoTDA's application-side
API. The constraint carries over unchanged: whatever is built must stay inside
the existing allowlist, and must not be able to silence a local alarm.

**Multiple devices and alarm tiering.** Every table is already keyed by
`device_id` and the ingest path enforces an allowlist, so the storage model
extends without migration. What is missing is grouping, per-device thresholds,
and an escalation policy — at present one device's `warning` is indistinguishable
from another's.

**A custom board.** The current build is a devkit with breakout modules across
two I²C buses. A single PCB removes the wiring as a failure mode and fixes the
buzzer's active level in hardware instead of a compile-time macro.

## Demo Script

The device carries the demo. Everything here works with the laptop closed.

1. Power on. The buzzer self-tests within a second; the OLED carousel shows
   readings, the verdict with its confidence, and the learned baseline range.
2. Warm the sensor by hand. The OLED moves to `TEMP RISING`.
3. Keep warming past 30 °C. The buzzer sounds and the OLED names the cause with
   real numbers: `ALARM  TEMP 31.2C LIMIT 30.0C`.
5. **Pull the Wi-Fi and do it again.** The buzzer sounds at the same speed with
   the same reason on screen. This is the argument for the whole architecture,
   and it takes ten seconds to make.
5. Stronger still: hold the sensor warm and press reset. The alarm fires while
   the device is still connecting to Wi-Fi — it never waited for the network.
6. Reconnect. Show the IoTDA console receiving `Environment` and `EdgeReasoning`
   properties, and `edge_assessments` in SQLite as a state transition timeline
   rather than a wall of duplicates.

Without hardware: `python3 scripts/simulate_sensor.py --interval 2` drives the
same path, and `--force-edge-state HARD_LIMIT` demonstrates the safety state.
