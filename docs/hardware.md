# Hardware Design

The edge device. It runs the entire decision path — sensing, filtering, baseline
learning, assessment, and display — without network involvement.

It runs no language model, and holds no API key, because it never calls one.

## Components

| Component | Role | Connection |
|---|---|---|
| ESP32-S3 | Controller: sampling, reasoning, OLED, Wi-Fi, MQTT/MQTTS, GPIO | — |
| SHT30 | Temperature and humidity sensing | `Wire`, SDA 17 / SCL 18, addr `0x44` |
| OLED | Readings, device verdict, link status | `Wire1`, SDA 38 / SCL 39, addr `0x3C` |
| Buzzer | Audible alert (**disabled by default**, see below) | GPIO 4, active-low |

## Device responsibilities

Implemented in `firmware/combined`:

- Sample the SHT30 over I²C on a dedicated 1 Hz FreeRTOS task.
- Check fixed hard limits on every sample, independent of network state.
- Median-filter readings before they reach the reasoning layers.
- Learn and persist an adaptive baseline (NVS namespace `envbaseline`).
- Assess a 12-sample window and produce an `EdgeAssessment`.
- Render readings, verdict, and link status on the OLED carousel.
- Publish to Huawei Cloud IoTDA over MQTT/MQTTS as a property report.
- POST the same assessment to the local backend, with exponential backoff.
- Poll and acknowledge allowlisted device commands.
- Keep NTP time synchronised, retrying every 5 minutes.

See [edge_reasoning.md](edge_reasoning.md) for the reasoning layers.

## Firmware variants

| Path | Purpose |
|---|---|
| `firmware/combined` | **The full system.** Edge reasoning + IoTDA + local backend. Use this one. |
| `firmware/iotda_mvp` | Minimal IoTDA MQTT activation test |
| `firmware/src` | Earlier local-backend-only prototype |

## I²C wiring

**The sensor and the display are on two separate I²C buses.** They do not share
one. Wiring them to a common bus will not work with this firmware.

| Device | Bus | SDA | SCL | Address | Defined in |
|---|---|---|---|---|---|
| SHT30 | `Wire` | GPIO 17 | GPIO 18 | `0x44` (falls back to `0x45`) | `sht30.h` |
| OLED | `Wire1` | GPIO 38 | GPIO 39 | `0x3C` | `oled.h` |

```text
ESP32-S3                SHT30              OLED
3.3V              ->    VIN          3.3V ->  VCC
GND               ->    GND          GND  ->  GND
GPIO 17 (SDA)     ->    SDA
GPIO 18 (SCL)     ->    SCL
GPIO 38 (SDA1)                            ->  SDA
GPIO 39 (SCL1)                            ->  SCL
```

Separate buses keep a hung display from taking the sensor down with it — the
1 Hz safety task must keep reading regardless of what the OLED is doing. Each
bus needs its own pull-ups; most SHT30 and OLED breakout modules already carry
them, so check before adding more.

Pin numbers live in `sht30.h` and `oled.h`, not in `config.h`. Change them there
if the board layout requires it.

## Buzzer

`ENABLE_BUZZER` defaults to `0`. With it off, `HttpClient::initActuators()`
configures the pin as `INPUT` — high-impedance — so a queued command cannot
energise a circuit that has not passed hardware verification.

```text
BUZZER_PIN (default GPIO 4) -> buzzer signal input
GND                         -> buzzer ground
```

To enable it, verify the module and wiring first, then define `ENABLE_BUZZER 1`
in `config.h`. Note that `BUZZER_ON_LEVEL` is `LOW`: the firmware assumes an
active-low module. Check yours before enabling.

**Until then, the OLED is the alert output**, and any documentation or
presentation should say so rather than describing a buzzer that will not sound.

## Command surface

The device accepts only these, and bounds them itself:

| Command | Effect |
|---|---|
| `buzzer_on` | Sound for `duration_ms` (0–30000, defaults to 1000) |
| `buzzer_off` | Stop |
| `oled:<text>` | Show a short message |

No general GPIO control is exposed. OLED text must be ASCII — there is no
Chinese font on the display, so non-ASCII renders as garbage.

## Configuration

Copy the template and fill it in locally:

```bash
cp firmware/combined/src/config.example.h firmware/combined/src/config.h
```

`config.h` is gitignored and must stay that way. It holds the Wi-Fi password,
the IoTDA device secret, and the backend URL. Key settings:

| Macro | Meaning |
|---|---|
| `DEVICE_ID` | Must match the backend allowlist (`devices.allowlist`) |
| `REPORT_INTERVAL_MS` | Reporting cycle, default 10000 (8,640 reports/day) |
| `SERVER_URL` | Local backend `/api/ingest` endpoint |
| `ENABLE_BUZZER` | 0 by default, see above |
| `BUZZER_PIN` | Default GPIO 4 |

## What the device must not do

- Do not store LLM API keys in firmware. It never calls a model, so it needs none.
- Do not call DeepSeek, Tongyi, Pangu, or OpenAI from the device.
- Do not run Ollama on the ESP32-S3.
- Do not expose GPIO control to a public network.
- Do not invent a private IoTDA property report format.
- Do not put the decision path behind a network call.
