# On-Device Reasoning

This is the core of the project. Every decision the system makes is made on the
ESP32-S3 itself, by deterministic code, with no network involved.

Source: [`firmware/combined/src/edge_reasoner.cpp`](../firmware/combined/src/edge_reasoner.cpp),
[`firmware/combined/src/adaptive_baseline.cpp`](../firmware/combined/src/adaptive_baseline.cpp),
and `applyAdaptiveAssessment()` in [`firmware/combined/src/main.cpp`](../firmware/combined/src/main.cpp).

## Why the decision lives on the chip

A cloud-decides architecture makes every alert depend on Wi-Fi, on the cloud
platform, on an API quota, and on a model's willingness to answer in the right
format. Each of those is a way for an alert to not happen.

Here, the chip decides. The network carries the result outward; it is never
consulted to produce it. If Wi-Fi drops, the OLED keeps showing correct verdicts
and the safety path keeps working. Nothing about the decision degrades.

The reasoning is also cheap enough to be honest about: a 12-sample ring buffer
and a handful of comparisons, on a microcontroller. There is no model on the
device, and none is needed.

The reasoner is fed on a fixed 10 s cadence (`EDGE_SAMPLE_INTERVAL_MS`) rather
than at the sensing rate, so its 12-sample window always spans the 2 minutes its
thresholds were tuned against. Feeding it faster would silently shrink the
window and change what those thresholds mean — the net-rise condition would
overtake the rate condition, raising the slope needed to call a rise "rapid".

## Two layers

```text
SHT30 sample
  -> safety task (1 s)          fixed hard limits, independent of everything
  -> median filter
  -> AdaptiveBaseline.observe() learns what "normal" means in THIS room
  -> EdgeReasoner.add/assess()  fixed thresholds + trend over 12 samples
  -> applyAdaptiveAssessment()  baseline overlays the fixed verdict
  -> EdgeAssessment             {state, severity, confidence, reason_code, reason}
```

`EdgeAssessment` is what the OLED renders, what goes to Huawei Cloud IoTDA as a
property, and what the local backend stores. All three see the same struct.

### Layer 1 — EdgeReasoner: fixed thresholds and trend

A 12-sample sliding window (`WINDOW_SIZE`). Below 4 samples (`MIN_BASELINE_SAMPLES`)
it reports `WARMUP` rather than guessing from a window it does not have.

Rules are evaluated in order; the first match wins.

| State | Severity | Condition | reason_code |
|---|---|---|---|
| `WARMUP` | info | fewer than 4 samples | `LEARNING_BASELINE` |
| `UNSTABLE` | warning | window range ≥ 4 °C or ≥ 15 %RH, or sample-to-sample step ≥ 2.5 °C or ≥ 10 %RH | `ERRATIC_SIGNAL` |
| `HEAT_HUMID_RISK` | warning | latest ≥ 30 °C **and** ≥ 70 %RH | `HOT_AND_HUMID` |
| `TEMP_RISING` | warning | slope ≥ 1.2 °C/min and net rise ≥ 0.8 °C | `RAPID_TEMP_RISE` |
| `HUMIDITY_RISING` | warning | slope ≥ 4 %RH/min and net rise ≥ 3 %RH | `RAPID_HUMIDITY_RISE` |
| `HIGH_TEMPERATURE` | watch | latest ≥ 30 °C | `TEMP_ABOVE_COMFORT` |
| `HIGH_HUMIDITY` | watch | latest ≥ 70 %RH | `HUMIDITY_ABOVE_COMFORT` |
| `NORMAL` | info | none of the above | `STABLE_ENVIRONMENT` |

`UNSTABLE` is checked first on purpose. If the signal itself cannot be trusted,
no conclusion drawn from it can be either — so the device says the readings are
erratic instead of reporting a confident number derived from noise.

Rate is judged per minute, not per sample. The same 8 °C change is routine over
an hour and physically implausible over eight seconds; only the slope separates
them.

### Layer 2 — AdaptiveBaseline: what is normal *here*

Fixed thresholds cannot know that a boiler room idles at 34 °C or that a cellar
sits at 78 %RH. `AdaptiveBaseline` learns the ambient range of the actual
installation and flags departures from it.

| Property | Value | Why |
|---|---|---|
| Warmup | 4 min, measured against `millis()` | Enough to characterise a room. Measured in time rather than samples so an adaptive reporting interval cannot change how much evidence it learns from |
| Band | center ± 3σ, clamped to [1.5, 5] °C and [5, 15] %RH | An unbounded band eventually accepts anything |
| Learning rate | 0.02 center, 0.05 deviation | Slow: ordinary drift must not drag the band along |
| Out-of-band samples | not learned | Otherwise a passing anomaly drags the baseline along with it |
| Sustained departure | relearn after 1 h out of band, likewise measured in time | Without it a relocated device is stuck in `BASELINE_SHIFT` forever — see below |
| Warmup clamping | ±3 °C / ±10 %RH per sample | One odd sample cannot define the initial center |
| Persistence | NVS blob, magic + version + config signature + checksum | Survives reboot; a changed config invalidates the old blob |

The learned band is always kept at least 0.5 °C and 2 %RH inside the hard
limits, so **learning can never widen the safety boundary**. This is the
property that makes an adaptive system safe to ship: the part that adapts and
the part that guarantees are separate, and the adaptive part is bounded by the
guaranteed one.

### Relearning after a move

Not learning from out-of-band samples protects the baseline from being dragged
along by an anomaly, but taken alone it has a trap: a device carried to a
different room sees *every* sample out of band, so nothing is learned, the band
never moves again, and it reports `BASELINE_SHIFT` forever. Nothing is written
back to NVS either — the persistence path is gated on `dirty_`, which only
learning sets.

So a departure sustained for `relearnAfterOutsideMs` — one hour, measured
against the clock rather than counted in samples — is taken as evidence that the
learned band describes somewhere else, and learning restarts from scratch. Any
in-band sample restarts that clock, so a brief excursion never triggers it.

Both this and the warmup are read from `millis()`, which is what makes the
adaptive reporting interval below safe to have: "an hour out of band" means an
hour whether the device is currently processing every 2 s or every 10 s. One
sample can credit at most `kMaxObserveDeltaMs` (60 s), so a stalled caller
cannot finish warmup in a single observation.

This costs nothing in safety, and it is worth being precise about why. Alarm
thresholds (`ALARM_TEMP_C`, `ALARM_HUMIDITY_PCT`) and hard limits are fixed
human-set values that are never learned, and the buzzer is driven from the raw
reading against those, not from the baseline. Relearning a warmer room changes
which readings are called a *pattern shift*. It cannot change which ones sound
the alarm.

Set `relearnAfterOutsideMs` to 0 to disable it and keep the band frozen
once it leaves — appropriate only if a permanently stuck `BASELINE_SHIFT` is
preferable to an adapted one.

### The overlay

`applyAdaptiveAssessment()` combines the two:

| Result | Severity | When | reason_code |
|---|---|---|---|
| `HARD_LIMIT` | warning | Fixed safety limit crossed — overrides everything | `FIXED_SAFETY_LIMIT` |
| `BASELINE_SHIFT` | watch | Outside the learned band, and the fixed layer said `NORMAL` | `TEMP_OUTSIDE_BASELINE`, `HUMIDITY_OUTSIDE_BASELINE`, `TEMP_HUMIDITY_OUTSIDE_BASELINE` |

The baseline only speaks when the fixed layer has nothing to say. A real
threshold breach is never downgraded to "pattern shift".

Hard limits (`AdaptiveBaselineConfig`): temperature ≤ −10 °C or ≥ 45 °C,
humidity ≤ 5 %RH or ≥ 95 %RH.

## The safety task

Hard-limit detection runs on its own FreeRTOS task at 1 Hz, pinned to a core,
separate from every reporting cycle.

- **Sampling is never delayed by network work.** MQTT reconnects, HTTP retries,
  and backoff cannot stretch the safety interval.
- **A stale sample blocks reporting.** Readings older than 1.5 s
  (`SAFETY_SAMPLE_MAX_AGE_MS`) are refused rather than sent, so a stalled task
  cannot cause a confident report of an old value.
- **Hard-limit transitions are latched** until the next cloud slot, so a
  condition that clears quickly is still reported — while the daily message
  budget stays predictable.
- **Clearing requires 3 consecutive safe samples** (`HARD_LIMIT_CLEAR_SAFE_SAMPLES`),
  which stops a value sitting on the boundary from oscillating.

## The local alarm

The assessment above explains; the alarm *acts*. Crossing either threshold
sounds the buzzer and writes the reason to the OLED:

```c
#define ALARM_TEMP_C 30.0f
#define ALARM_HUMIDITY_PCT 70.0f
```

These sit below the hard limits (45 °C / 95 %RH) on purpose. Hard limits are a
safety boundary that learning may never widen; these are the everyday alarm
point. Cupping the sensor trips humidity first — palm skin is near saturation,
while heat has to conduct through the housing.

Three properties make this the part of the system that cannot be broken from
outside:

**It runs in the 1 Hz safety task, not `loop()`.** `loop()` does not begin until
`setup()` finishes, and setup blocks on Wi-Fi (20 s timeout), NTP, and MQTT. An
environment already over threshold at power-on would wait more than thirty
seconds for a beep. The safety task starts before Wi-Fi, so the alarm is
genuinely independent of the network — verified on hardware, where `[ALARM]`
appears in the log while Wi-Fi is still connecting.

**A network command cannot silence it.** While the alarm is active, remote
`buzzer_on` / `buzzer_off` — from the backend queue or from IoTDA — are refused
and logged.

**Clearing needs 3 consecutive safe samples**, so a value resting on the
threshold does not chatter on and off.

The OLED carries the reason rather than a bare label, because someone standing
in front of the device needs to know which quantity, how far, and past what:

```text
! DEVICE ALERT !

ALARM  TEMP 31.2°C
LIMIT 30.0°C
```

The same string is printed to the serial log, so an alert can be diagnosed
without looking at the screen.

## What the device sends

The assessment goes out over two independent paths, unchanged, each on its own
interval: the local backend every `senseIntervalMs` (2–10 s, adaptive, free),
and IoTDA every `CLOUD_INTERVAL_MS` (60 s, fixed, metered).

### The adaptive reporting interval

`senseIntervalMs` drives the median filter, `AdaptiveBaseline::observe()`, the
OLED refresh and the local POST. It is **not** the sampling rate: the SHT30 is
still read once a second by the safety task, unchanged, because the sampling
rate *is* the alarm latency. What adapts is how often that reading is processed
and reported.

The signal is the deviation the baseline has already learned — how much this
room normally moves. A calm room does not need the same processing rate as one
that is changing, so the interval stretches toward 10 s when the learned
deviation is small.

| Rule | Value | Why |
|---|---|---|
| Floor | 2 s | SHT30 τ63 ≈ 2 s; processing faster adds no information |
| Ceiling | 10 s | `EDGE_SAMPLE_INTERVAL_MS` — reporting slower than the reasoner is fed would starve the 12-sample window |
| Preempt | state change, severity ≠ `info`, baseline not ready, out of band, hard limit | Back to the floor immediately; the learned deviation is a slow variable and must not be what decides that things have calmed down |
| Fast hold | 30 s after any preempt | Same reason, from the other side: just after an event is not the moment to conclude it is over |
| Slowing | +500 ms per cycle | Gradual, and interruptible at any point |

Speeding up is immediate, slowing down is gradual. The asymmetry puts the cost
of being wrong on the "processed a few more times than needed" side rather than
the "noticed late" side.

Both bounds come from physics rather than tuning, and the whole feature is only
safe because the alarm does not depend on this path — an early decision to run
hard limits on their own 1 Hz task is what allows this one to vary at all.

**Huawei Cloud IoTDA** — one MQTT message, two services:

```json
{"services": [
  {"service_id": "Environment",   "properties": {"temperature": 26.5, "humidity": 58.0}},
  {"service_id": "EdgeReasoning", "properties": {"state": "NORMAL", "severity": "info",
                                                 "confidence": 0.92,
                                                 "reason_code": "STABLE_ENVIRONMENT"}}
]}
```

**Local backend** — `POST /api/ingest`:

```json
{"device_id": "esp32s3-001", "timestamp": 1234, "temperature": 26.5, "humidity": 58.0,
 "edge": {"state": "NORMAL", "severity": "info", "confidence": 0.92,
          "reason_code": "STABLE_ENVIRONMENT", "reason": "Temperature and humidity are stable."}}
```

Neither receiver recomputes the verdict. The backend stores it in
`edge_assessments` and serves it to the dashboard as-is — what the web page shows
is what the chip concluded.

## Testing without hardware

The baseline logic has host tests that need no board — no Arduino headers are
involved, so a plain compiler runs them:

```bash
c++ -std=c++17 -I firmware/combined/src \
    firmware/combined/test_host/adaptive_baseline_test.cpp \
    firmware/combined/src/adaptive_baseline.cpp -o /tmp/baseline_test && /tmp/baseline_test
```

[`scripts/simulate_sensor.py`](../scripts/simulate_sensor.py) contains a Python
port of `EdgeReasoner` with the same constants, so the backend, database, and
dashboard can be exercised end to end with nothing powered on:

```bash
python3 scripts/simulate_sensor.py --interval 2
```

The overlay states are not simulated — learning a real baseline takes 24 samples
of genuine ambient data. Force them directly instead:

```bash
python3 scripts/simulate_sensor.py --force-edge-state HARD_LIMIT
```

The alarm itself is device-side and needs the board. The quickest check without
heating anything is to set `ALARM_TEMP_C` temporarily below the current room
temperature, flash, confirm the buzzer and the `[ALARM]` log line, then restore
it.

If the constants in `edge_reasoner.cpp` change, update the Python mirror in the
same commit. A silently diverged simulator tests nothing.

## Related

- [architecture.md](architecture.md) — where this sits in the system
- [data_flow.md](data_flow.md) — the full path from sample to display
- [llm_reasoning.md](llm_reasoning.md) — the optional narration layer, which decides nothing
