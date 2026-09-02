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
| Buzzer | Local threshold alarm, verified working | GPIO 4, active-low, `ENABLE_BUZZER 1` |

## Device responsibilities

Implemented in `firmware/combined`:

- Sample the SHT30 over I²C on a dedicated 1 Hz FreeRTOS task.
- Check fixed hard limits on every sample, independent of network state.
- Sound the buzzer and show the reason on the OLED when a local threshold is
  crossed — no network anywhere in that path.
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

```text
BUZZER_PIN (GPIO 4) -> buzzer signal input
GND                 -> buzzer ground
```

Hardware-verified on an active-low module: `BUZZER_ACTIVE_LEVEL LOW`,
`ENABLE_BUZZER 1`. `config.example.h` still ships with `ENABLE_BUZZER 0`,
because nobody else's module and wiring have been checked, and with it off
`HttpClient::initActuators()` holds the pin at `INPUT` — high-impedance — so no
queued command can energise an unverified circuit.

**Determine the active level before enabling.** Guessing wrong means the buzzer
sounds continuously from power-on and cannot be silenced. With VCC and GND
connected, touch the module's I/O pin to GND — if it sounds, it is `LOW`; if it
only sounds against 3V3, set `BUZZER_ACTIVE_LEVEL HIGH`.

On boot the firmware emits a short self-test beep, which confirms wiring and
active level in one second without needing the network, a threshold, or a
command:

```text
[BUZZER] Self-test beep
```

## Local threshold alarm

The one alarm path that no network failure can break. Crossing either threshold
sounds the buzzer and puts the reason on the OLED, on the device, with nothing
in between.

```c
#define ALARM_TEMP_C 30.0f
#define ALARM_HUMIDITY_PCT 80.0f
```

These sit deliberately below the `AdaptiveBaseline` hard limits (45 °C / 95 %RH):
the hard limits are a safety boundary, while these are the everyday alarm point,
and 30 °C is about what cupping the sensor in your hand produces — so the alarm
is demonstrable without heating anything.

Evaluation runs inside the **1 Hz safety task**, not the main loop. That
distinction matters: `loop()` does not run until `setup()` finishes, and setup
blocks on Wi-Fi (20 s timeout), NTP, and MQTT. An environment already over
threshold at power-on would otherwise wait more than thirty seconds for a beep.
The safety task starts before Wi-Fi, so the alarm is genuinely independent of
the network.

Clearing requires 3 consecutive safe samples, so a value resting on the
threshold does not chatter.

While the local alarm is active, remote `buzzer_on` / `buzzer_off` commands —
from the backend queue or from IoTDA — are refused and logged. A real alarm is
not something a network message gets to switch off.

## Alert display

`OLED::showAlert()` renders under a `! DEVICE ALERT !` header with room for
5 lines of 21 characters, so alerts carry their reason and the actual numbers
rather than a bare label:

| Condition | On screen |
|---|---|
| Temperature alarm | `ALARM  TEMP 31.2°C LIMIT 30.0°C` |
| Humidity alarm | `ALARM  HUMIDITY 82.5% LIMIT 80.0%` |
| Hard limit, upper | `HARD LIMIT  TEMP 46.1°C MAX 45.0°C` |
| Hard limit, lower | `HARD LIMIT  TEMP -12.0°C MIN -10.0°C` |
| Sensor faults | `SHT30 OFFLINE`, `SHT30 INVALID DATA`, `SENSOR DRIFT` |
| Link status | `WIFI OFFLINE`, `MQTT OFFLINE` — the cloud record path. The local recorder is deliberately **not** shown: it is a verification sink whose absence changes nothing the device does. |

The font covers `0x20`–`0x5B`, with `[` remapped to the degree glyph (see the
tail of `font5x7` in `oled.cpp`), so digits, `%`, `.` and `°` all render. The
serial log prints the same string that goes to the screen, so alerts can be
diagnosed without standing in front of the device.

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
| `SENSE_INTERVAL_MS` | Reasoning, OLED and local backend cycle, default 2000. Not metered — shorten it to make the display react faster |
| `CLOUD_INTERVAL_MS` | IoTDA publish only, default 60000 (1,440 msg/day, 14% of a 10,000/day free tier) |
| `SERVER_URL` | Local backend `/api/ingest` endpoint. Must be reachable from the **device's** subnet — a laptop on a different network is the usual cause of `[HTTP] POST failed code=-1` |
| `ENABLE_BUZZER` | 0 in the example; 1 once the module is verified |
| `BUZZER_ACTIVE_LEVEL` | `LOW` or `HIGH` — the level that makes your module sound |
| `BUZZER_PIN` | GPIO 4 |
| `ALARM_TEMP_C` / `ALARM_HUMIDITY_PCT` | Local alarm thresholds |

## What the device must not do

- Do not store LLM API keys in firmware. It never calls a model, so it needs none.
- Do not call DeepSeek, Tongyi, Pangu, or OpenAI from the device.
- Do not run Ollama on the ESP32-S3.
- Do not expose GPIO control to a public network.
- Do not invent a private IoTDA property report format.
- Do not put the decision path behind a network call.
