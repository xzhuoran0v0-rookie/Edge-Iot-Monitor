# System Architecture

An environment monitoring system whose decisions are made on the device, by
deterministic code, with no network dependency.

The organising principle:

```text
The ESP32-S3 decides.
The network reports the decision outward.
The cloud and the LLM explain and display it. Neither decides anything.
```

## Layers

```text
+--------------------------------------------------------------+
| ESP32-S3                                        DECIDES       |
|   SHT30 -> 1 Hz safety task -> median filter                  |
|   AdaptiveBaseline (learned normal range, NVS-persisted)      |
|   EdgeReasoner (12-sample window, fixed thresholds + trend)   |
|   -> EdgeAssessment {state, severity, confidence, reason}     |
|   OLED shows it immediately, network or not                   |
|   Buzzer sounds on a local threshold, ~1 s, no network        |
+---------------------------+----------------------------------+
                            |
        +-------------------+-------------------+
        | MQTT/MQTTS                            | HTTP
        v                                       v
+---------------------------+   +------------------------------+
| Huawei Cloud IoTDA        |   | Local backend (C++)  STORES  |
|   Device access           |   |   POST /api/ingest           |
|   Property report:        |   |   SQLite: readings,          |
|     Environment           |   |     edge_assessments,        |
|     EdgeReasoning         |   |     anomaly_events           |
|   Command downlink        |   |   IQR outlier detection      |
+---------------------------+   |   Command queue              |
                                |   GET /api/readings          |
                                +---------------+--------------+
                                                |
                        +-----------------------+------------+
                        |                                    |
                        v                                    v
        +-------------------------------+   +--------------------------+
        | Web dashboard      DISPLAYS   |   | LLM narration  EXPLAINS  |
        |   Live readings and trends    |   |   DeepSeek, or local     |
        |   The device's verdict, as-is |   |   Ollama fallback        |
        +-------------------------------+   |   Optional; off by       |
                                            |   default; decides       |
                                            |   nothing                |
                                            +--------------------------+
```

## Responsibility split

| Layer | Responsibility | Can it decide? |
|---|---|---|
| ESP32-S3 | Sense, filter, learn baseline, assess, display, sound the alarm | **Yes — the only layer that does** |
| Huawei Cloud IoTDA | Device access, MQTT transport, property storage, command channel | No |
| Local backend | Persist readings and verdicts, IQR outlier detection, serve the dashboard, queue commands | No |
| Web dashboard | Show live data and the device's verdict unchanged | No |
| LLM (DeepSeek / Ollama) | Turn a state change into a sentence a human can read | No |

The single most important row is the last one. The LLM is given the system's
conclusions and asked to explain them — it is never asked what to conclude. Its
prompt says so explicitly, and no code path converts model output into an alert
or a command.

## Why this shape

**Alerts must not depend on connectivity.** The decision path contains no
network call. Wi-Fi down, cloud unreachable, API quota gone — the OLED still
shows the right state and the safety task still runs at 1 Hz.

**Adaptivity must not erode safety.** `AdaptiveBaseline` learns what is normal
for the room it is installed in, but its bands are clamped inside immutable hard
limits. Learning can narrow attention, never widen the safety boundary.

**Latency is bounded by hardware, not by an API.** Hard-limit detection is one
sample away — 1 second — not one round trip and one model response away. The
alarm is evaluated inside the 1 Hz safety task rather than the main loop, so it
fires before Wi-Fi, NTP and MQTT have even finished connecting, and a network
command cannot switch it off while it is active.

**No key ever reaches the device.** The ESP32-S3 holds no LLM credential
because it never calls a model. This is a consequence of the architecture, not
a mitigation bolted onto it.

**Cost is zero per decision.** 8,640 assessments per device per day, none of
them billable.

## Implementation status

| Component | Status |
|---|---|
| On-device reasoning (EdgeReasoner + AdaptiveBaseline) | Implemented, host-tested |
| 1 Hz safety task with hard limits | Implemented |
| OLED display and status pages | Implemented |
| IoTDA MQTT property report (`Environment` + `EdgeReasoning`) | **Verified on hardware** — connects over MQTTS and publishes both services |
| Local backend ingest, storage, IQR, command queue | Implemented |
| `GET /api/readings` and web dashboard | Implemented |
| LLM narration (DeepSeek primary, Ollama fallback) | Implemented, disabled by default |
| Backend → IoTDA command downlink (`CloudSync`) | **Not implemented** — skeleton only |
| Local threshold alarm (buzzer + OLED reason) | **Verified on hardware** — evaluated in the 1 Hz safety task |
| Buzzer output | Verified active-low; `ENABLE_BUZZER 1` locally, `0` in the example config |

`CloudSync` is the one row that does not work: it has no send path. It is stated
here rather than implied to work. The buzzer ships disabled in the example
config for the same reason it was disabled here until it was tested — nobody
else's module and wiring have been checked, and guessing the active level wrong
makes it sound continuously from power-on.

## Related documents

- [edge_reasoning.md](edge_reasoning.md) — the decision layer in detail
- [hardware.md](hardware.md) — wiring and device responsibilities
- [data_flow.md](data_flow.md) — end-to-end path and failure behaviour
- [backend_service.md](backend_service.md) — backend responsibilities and HTTP API
- [cloud_iotda.md](cloud_iotda.md) — IoTDA topics and product model
- [llm_reasoning.md](llm_reasoning.md) — the narration layer
- [competition_writeup.md](competition_writeup.md) — presentation wording
