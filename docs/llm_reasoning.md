# LLM Narration

The LLM in this project explains. It does not decide.

That is not a limitation worked around — it is the design. Detection is
deterministic and has already acted before the model is ever called. The model's
only job is to turn a state change into a sentence a person can read.

## Position in the system

```text
ESP32-S3 decides            <- deterministic, on-chip, no network
  -> backend stores the verdict
  -> IQR flags outliers     <- deterministic, server-side
  -> [state changed?]
       -> LLM narrates it   <- optional, explains only
  -> dashboard displays
```

Nothing downstream of the model reads its output as a command. There is no code
path that turns model text into an alert, a GPIO change, or an IoTDA command.
The prompt states this to the model directly:

> You do NOT control alarms. Alarms are handled deterministically and have
> already acted before you see this.

Telling the model its output is not load-bearing has a practical effect: it
stops hedging and stops inflating severity to be safe, because being wrong
cannot cause harm. It writes what the numbers say.

## Narrate on change, never on a count

Earlier this fired every N records. That re-analysed identical steady-state
data forever: the model could only repeat its own input back, and it burned API
quota doing it.

Narration now fires only on a state change:

| Trigger | Cooldown applies? |
|---|---|
| First report from a device (baseline) | No |
| IQR anomaly detector flagged the batch | Yes |
| Crossed a warn-band edge, in either direction | No |
| Drifted ≥ `temp_delta_c` / `humidity_delta` since the **last narration** | Yes |

Two details that matter:

**Band crossings bypass the cooldown, anomalies do not.** Band state is latched,
so a crossing fires once on entry and once on exit — sustained heat cannot
re-trigger it. An IQR anomaly is different: after a genuine level shift, IQR
flags every subsequent reading until the sliding window refills. Without the
cooldown that stretch becomes a wall of narration.

**Drift is measured from the last narration, not the last reading.** Otherwise a
slow ramp never trips a per-reading threshold and is never mentioned at all.

Steady state produces no call and therefore costs nothing.

## What the model receives

Aggregated statistics, not raw rows: per sensor type, the latest value, min,
max, mean, net change, sample count, the window span, and the **rate per
minute**.

The rate is the point. Without it the model cannot distinguish "8 °C over two
hours" from "8 °C over eight seconds" — the first is routine, the second is not
physically plausible for room air and usually means someone touched the sensor.

The prompt also lists what the system already does, so the model does not
recommend adding a median filter to a device that has had one all along.

## Output

Strict JSON, ASCII only — the OLED has no font for anything else:

```json
{
  "situation": "one sentence: what is happening, with numbers",
  "severity": "normal | watch | concern | urgent",
  "explanation": "2-3 sentences: why the data looks this way",
  "suggested_action": "what a person should do, or 'none'",
  "verdict": "max 20 chars, for a small display"
}
```

`severity` here describes the situation for a human reader. It triggers nothing.
The device's own `severity` is the one that matters, and it was computed on the
chip.

## Backends

| Priority | Backend | Notes |
|---|---|---|
| 1 | DeepSeek (`/chat/completions`, OpenAI-compatible) | Used when `deepseek.enabled` and an API key is present |
| 2 | Local Ollama | Automatic fallback on any cloud failure |
| — | None | Narration is skipped; **no alert is affected** |

The fallback chain is about keeping the *explanation* available. It is not a
safety mechanism, because the safety mechanism is on the device and never
involved a model.

Configure in `config/config.yaml`; `DEEPSEEK_API_KEY` in the environment takes
priority over the file. `ai.enabled: false` disables narration entirely.

## Key handling

The API key lives only in server config or a server environment variable. The
ESP32-S3 has no key — not because it is withheld, but because the device never
calls a model at all. There is nothing on the device to extract.

## Data Q&A

`POST /api/prompt` answers free-text questions about recent readings. It is a
convenience feature for the dashboard, outside the monitoring path entirely.
Note that it currently runs synchronously on the request thread and is not
covered by `ai.enabled`.
