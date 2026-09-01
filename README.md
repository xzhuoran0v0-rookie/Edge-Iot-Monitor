# Edge IoT Monitor

An environment monitoring system whose intelligence runs **on the device**.

An ESP32-S3 with an SHT30 sensor learns what "normal" means in the room it is
installed in, and decides for itself — in deterministic code, with no network
call anywhere in the decision path — whether conditions have changed. Huawei
Cloud IoTDA and a local C++ service carry that decision outward. A language
model is available to explain it in plain language, and is never asked what to
conclude.

```text
SHT30
  -> 1 Hz safety task            fixed hard limits, independent of everything
  -> median filter
  -> AdaptiveBaseline            learns this room's normal range, persisted to NVS
  -> EdgeReasoner                12-sample window, thresholds + per-minute rates
  -> EdgeAssessment              {state, severity, confidence, reason}
       |
       +-> OLED                  immediate, works with no network
       +-> buzzer                local threshold alarm, ~1 s, no network
       +-> Huawei Cloud IoTDA    MQTT property report
       +-> local backend         SQLite, IQR, web dashboard
             +-> LLM narration   explains a state change; decides nothing
```

Pull the Wi-Fi and the device keeps assessing correctly. That is the point of
the architecture, and it demonstrates in five seconds.

## Why not decide in the cloud

A cloud-decides design makes every alert depend on connectivity, on a platform,
on an API quota, and on a model returning parseable output. Each is a way for an
alert not to happen — and they fail exactly when a monitor is least able to
afford it.

Deciding on the chip costs one ring buffer and a handful of comparisons every
10 seconds. In exchange:

| | On-device | Cloud-decides |
|---|---|---|
| Works with no network | Yes | No |
| Hard-limit latency | 1 s | Round trip + inference |
| Cost per decision | Zero | Per API call |
| Credential on device | None needed | Key or token |
| Adapts to the room | Yes, bounded by hard limits | Depends on the prompt |

## Hardware

| Component | Role | Connection |
|---|---|---|
| ESP32-S3 | Sampling, reasoning, OLED, Wi-Fi/MQTT, GPIO | — |
| SHT30 | Temperature and humidity | `Wire`, SDA 17 / SCL 18, addr `0x44` |
| OLED | Readings, verdict, link status | `Wire1`, SDA 38 / SCL 39, addr `0x3C` |
| Buzzer | Local threshold alarm | GPIO 4, active-low, verified |

The sensor and display sit on **separate I²C buses**, so a hung display cannot
stall the 1 Hz safety task. See [hardware.md](docs/hardware.md) before wiring.

## Documentation

- [edge_reasoning.md](docs/edge_reasoning.md) — **the core**: how the device decides
- [architecture.md](docs/architecture.md) — layers, responsibilities, implementation status
- [data_flow.md](docs/data_flow.md) — end-to-end path and failure behaviour
- [backend_service.md](docs/backend_service.md) — backend modules and full HTTP API
- [hardware.md](docs/hardware.md) — wiring, firmware variants, configuration
- [cloud_iotda.md](docs/cloud_iotda.md) — IoTDA topics, product model, commands
- [llm_reasoning.md](docs/llm_reasoning.md) — the narration layer
- [competition_writeup.md](docs/competition_writeup.md) — presentation wording

## Quick start (no hardware needed)

Everything below runs on a laptop with nothing powered on.

```bash
cp config/config.example.yaml config/config.yaml
```

Build and run the backend from the repository root:

```bash
cmake -S . -B build-cmake -DEDGE_BUILD_SERVER=ON && cmake --build build-cmake
```

```bash
./build-cmake/backend/edge_server
```

Feed it simulated readings. The simulator includes a Python port of the
firmware's `EdgeReasoner`, so the payloads match what a real board sends:

```bash
python3 scripts/simulate_sensor.py --interval 2
```

Open <http://localhost:8080> for the live dashboard.

To demonstrate a state the simulator does not reach on its own:

```bash
python3 scripts/simulate_sensor.py --force-edge-state HARD_LIMIT
```

Run the backend smoke tests:

```bash
python3 scripts/test_backend_api.py
```

## Frontend development

The backend serves the built dashboard from `frontend/dist`. For live reload:

```bash
cd frontend && npm install && npm run dev
```

Vite proxies `/api` to `localhost:8080`. Rebuild with `npm run build` before
committing — `frontend/dist` is checked in so the backend can serve it standalone.

## Firmware

```bash
python3 -m pip install --user -r requirements-dev.txt
```

```bash
cp firmware/combined/src/config.example.h firmware/combined/src/config.h
```

```bash
cd firmware/combined && pio run
```

`firmware/combined` is the full system. `firmware/iotda_mvp` is a minimal IoTDA
activation test, and `firmware/src` is an earlier local-only prototype.

`config.h` holds the Wi-Fi password, the IoTDA device secret, and the backend
URL. It is gitignored and must stay that way.

## Implementation status

| Component | Status |
|---|---|
| On-device reasoning, baseline learning, safety task | Implemented, host-tested |
| OLED display | Implemented |
| IoTDA MQTT property report | **Verified on hardware** — connects and publishes `Environment` + `EdgeReasoning` |
| Backend ingest, storage, IQR, command queue, dashboard API | Implemented |
| Web dashboard | Implemented |
| LLM narration | Implemented, disabled by default |
| Backend → IoTDA command downlink (`CloudSync`) | **Not implemented** — skeleton only |
| Local threshold alarm (buzzer + OLED reason) | **Verified on hardware** — fires from the 1 Hz safety task, before the network is up |
| Buzzer output | Verified active-low; `ENABLE_BUZZER 1` locally, `0` in the example config |

## Security boundaries

- The decision path contains no network call, no cloud dependency, and no model.
- Hard safety limits are immutable; adaptive bands are clamped inside them and
  cannot widen them.
- The ESP32-S3 stores no LLM API key — it never calls a model.
- Device commands are allowlisted with a bounded duration; no arbitrary GPIO
  control is exposed.
- Ingest enforces a device allowlist; the command API can require a key.
- Real credentials live only in `config/config.yaml`, `config.h`, or server
  environment variables. None of these are committed — and none of them belong
  in a submission archive either.
- LLM output is never converted into a command.

## License

MIT
