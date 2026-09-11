#!/usr/bin/env python3
"""
simulate_sensor.py — Sensor Data Simulator
===========================================
Generates realistic sensor readings and POSTs them to the C++ backend,
allowing full backend + AI pipeline testing WITHOUT any physical hardware.

Usage:
    python3 scripts/simulate_sensor.py [--interval 2] [--device esp32-s3-001]
    python3 scripts/simulate_sensor.py --stdout --format iotda
"""

import argparse
import json
import math
import random
import sys
import time
import urllib.request
import urllib.error
from collections import deque
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# Config (override via CLI args)
# ---------------------------------------------------------------------------
BACKEND_URL  = "http://127.0.0.1:8080/api/ingest"
DEVICE_ID    = "esp32s3-001"
INTERVAL_SEC = 2.0   # seconds between readings
TIMEOUT_SEC  = 30.0  # HTTP timeout (LLM analysis may take time)

# ---------------------------------------------------------------------------
# Realistic sensor simulation with occasional anomalies
# ---------------------------------------------------------------------------
def generate_reading(t: float, inject_anomaly: bool = False) -> dict:
    """
    Simulates temperature + humidity with a slow sinusoidal drift
    to mimic real environmental variation.
    """
    base_temp = 25.0 + 3.0 * math.sin(t / 60.0)   # ~8°C swing over 2 min
    base_hum  = 60.0 + 8.0 * math.cos(t / 90.0)

    temperature = round(base_temp + random.gauss(0, 0.3), 2)
    humidity    = round(base_hum  + random.gauss(0, 0.5), 2)

    if inject_anomaly:
        # Spike one metric significantly
        if random.random() < 0.5:
            temperature += random.choice([-8.0, +10.0])
            print(f"  [!] Injecting temperature anomaly → {temperature}°C")
        else:
            humidity += random.choice([-20.0, +25.0])
            print(f"  [!] Injecting humidity anomaly → {humidity}%")

    return {
        "device_id":   DEVICE_ID,
        "timestamp":   int(datetime.now(timezone.utc).timestamp()),
        "temperature": temperature,
        "humidity":    max(0.0, min(100.0, humidity)),
    }


# ---------------------------------------------------------------------------
# On-device reasoning mirror
# ---------------------------------------------------------------------------
# Port of firmware/combined/src/edge_reasoner.cpp. The real assessment is
# produced on the ESP32; this reproduces it byte-for-byte in the payload so the
# backend, database, and dashboard can be exercised with no board powered on.
#
# Keep the constants below in sync with edge_reasoner.cpp — if they drift, this
# simulator quietly stops testing what the firmware actually sends.
#
# Scope: this mirrors EdgeReasoner only. On the real device, main.cpp's
# applyAdaptiveAssessment() overlays AdaptiveBaseline on top and can replace the
# verdict with HARD_LIMIT or BASELINE_SHIFT. Learning a baseline takes 24 warmup
# samples of real ambient data, so reproducing it here would not be meaningful —
# use --force-edge-state to exercise those two states instead.
# ---------------------------------------------------------------------------
WINDOW_SIZE = 12
MIN_BASELINE_SAMPLES = 4
HIGH_TEMP_C = 30.0
HIGH_HUMIDITY_PCT = 70.0
RAPID_TEMP_C_PER_MIN = 1.2
RAPID_HUMIDITY_PCT_PER_MIN = 4.0
UNSTABLE_TEMP_RANGE_C = 4.0
UNSTABLE_HUMIDITY_RANGE_PCT = 15.0
UNSTABLE_TEMP_STEP_C = 2.5
UNSTABLE_HUMIDITY_STEP_PCT = 10.0


# States produced by main.cpp's adaptive overlay rather than by EdgeReasoner.
# Exposed through --force-edge-state so the dashboard path for them can be
# demonstrated and verified without waiting on real ambient conditions.
OVERLAY_STATES = {
    "HARD_LIMIT": ("warning", 0.95, "FIXED_SAFETY_LIMIT",
                   "A fixed environmental safety limit was crossed."),
    "BASELINE_SHIFT": ("watch", 0.78, "TEMP_OUTSIDE_BASELINE",
                       "Temperature moved outside the learned normal range."),
}


class EdgeReasoner:
    """Mirror of the firmware's EdgeReasoner sliding-window assessment."""

    def __init__(self) -> None:
        self.samples: deque = deque(maxlen=WINDOW_SIZE)

    def add(self, temperature: float, humidity: float, captured_at_s: float) -> None:
        self.samples.append((temperature, humidity, captured_at_s))

    @staticmethod
    def _clamp(value: float) -> float:
        return round(max(0.0, min(0.99, value)), 2)

    def assess(self) -> dict:
        def result(state, severity, confidence, reason_code, reason):
            return {
                "state": state,
                "severity": severity,
                "confidence": self._clamp(confidence),
                "reason_code": reason_code,
                "reason": reason,
            }

        count = len(self.samples)
        if count < MIN_BASELINE_SAMPLES:
            progress = count / MIN_BASELINE_SAMPLES
            return result("WARMUP", "info", 0.35 + progress * 0.25,
                          "LEARNING_BASELINE", "Collecting a local trend baseline.")

        temps = [s[0] for s in self.samples]
        hums = [s[1] for s in self.samples]
        first, last = self.samples[0], self.samples[-1]

        temp_range = max(temps) - min(temps)
        hum_range = max(hums) - min(hums)
        max_temp_step = max(abs(b - a) for a, b in zip(temps, temps[1:]))
        max_hum_step = max(abs(b - a) for a, b in zip(hums, hums[1:]))

        elapsed_min = max(0.0, last[2] - first[2]) / 60.0
        temp_slope = (last[0] - first[0]) / elapsed_min if elapsed_min > 0 else 0.0
        hum_slope = (last[1] - first[1]) / elapsed_min if elapsed_min > 0 else 0.0

        if (temp_range >= UNSTABLE_TEMP_RANGE_C or
                hum_range >= UNSTABLE_HUMIDITY_RANGE_PCT or
                max_temp_step >= UNSTABLE_TEMP_STEP_C or
                max_hum_step >= UNSTABLE_HUMIDITY_STEP_PCT):
            margin = max(temp_range / UNSTABLE_TEMP_RANGE_C,
                         hum_range / UNSTABLE_HUMIDITY_RANGE_PCT)
            return result("UNSTABLE", "warning", 0.62 + margin * 0.12,
                          "ERRATIC_SIGNAL", "Readings are changing too sharply to trust.")

        if last[0] >= HIGH_TEMP_C and last[1] >= HIGH_HUMIDITY_PCT:
            margin = max((last[0] - HIGH_TEMP_C) / 8.0,
                         (last[1] - HIGH_HUMIDITY_PCT) / 25.0)
            return result("HEAT_HUMID_RISK", "warning", 0.72 + margin * 0.20,
                          "HOT_AND_HUMID", "Temperature and humidity are both elevated.")

        if temp_slope >= RAPID_TEMP_C_PER_MIN and last[0] - first[0] >= 0.8:
            return result("TEMP_RISING", "warning", 0.65 + temp_slope / 12.0,
                          "RAPID_TEMP_RISE", "Temperature is rising rapidly.")

        if hum_slope >= RAPID_HUMIDITY_PCT_PER_MIN and last[1] - first[1] >= 3.0:
            return result("HUMIDITY_RISING", "warning", 0.65 + hum_slope / 30.0,
                          "RAPID_HUMIDITY_RISE", "Humidity is rising rapidly.")

        if last[0] >= HIGH_TEMP_C:
            return result("HIGH_TEMPERATURE", "watch",
                          0.68 + (last[0] - HIGH_TEMP_C) / 15.0,
                          "TEMP_ABOVE_COMFORT", "Temperature is above the comfort threshold.")

        if last[1] >= HIGH_HUMIDITY_PCT:
            return result("HIGH_HUMIDITY", "watch",
                          0.68 + (last[1] - HIGH_HUMIDITY_PCT) / 35.0,
                          "HUMIDITY_ABOVE_COMFORT", "Humidity is above the comfort threshold.")

        stability = 1.0 - min(1.0, temp_range / UNSTABLE_TEMP_RANGE_C * 0.5 +
                                   hum_range / UNSTABLE_HUMIDITY_RANGE_PCT * 0.5)
        return result("NORMAL", "info", 0.70 + stability * 0.25,
                      "STABLE_ENVIRONMENT", "Temperature and humidity are stable.")


def to_iotda_property_report(payload: dict, service_id: str = "Environment") -> dict:
    return {
        "services": [
            {
                "service_id": service_id,
                "properties": {
                    "temperature": payload["temperature"],
                    "humidity": payload["humidity"],
                },
            }
        ]
    }


def post_reading(payload: dict, url: str, timeout_sec: float) -> bool:
    data = json.dumps(payload).encode("utf-8")
    req  = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            return resp.status == 200
    except urllib.error.URLError as e:
        print(f"  [ERR] POST failed: {e.reason}")
        return False


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="IoT Sensor Data Simulator")
    parser.add_argument("--interval", type=float, default=INTERVAL_SEC)
    parser.add_argument("--device",   type=str,   default=DEVICE_ID)
    parser.add_argument("--url",      type=str,   default=BACKEND_URL)
    parser.add_argument("--timeout",  type=float, default=TIMEOUT_SEC, help="HTTP timeout seconds")
    parser.add_argument("--count",    type=int,   default=0, help="0 = run forever")
    parser.add_argument(
        "--format",
        choices=["local-http", "iotda"],
        default="local-http",
        help="Payload format. local-http matches the current backup backend; iotda prints Huawei Cloud IoTDA services payloads.",
    )
    parser.add_argument("--service-id", type=str, default="Environment", help="IoTDA product model service_id")
    parser.add_argument(
        "--no-edge",
        action="store_true",
        help="Omit the on-device reasoning result. By default the simulator mirrors "
             "the firmware's EdgeReasoner and includes an 'edge' object, matching "
             "what a real ESP32 sends.",
    )
    parser.add_argument(
        "--force-edge-state",
        choices=sorted(OVERLAY_STATES),
        help="Always report this state instead of running the reasoner. These two "
             "come from the device's AdaptiveBaseline overlay, which this script "
             "does not simulate.",
    )
    parser.add_argument(
        "--stdout",
        action="store_true",
        help="Print one JSON payload per line (no HTTP POST); useful for piping into C++ ingest tools or checking IoTDA payloads.",
    )
    args = parser.parse_args()

    # Keep stdout clean when --stdout is used (so it can be piped to other tools).
    out = sys.stderr if args.stdout else sys.stdout

    print("Simulator started", file=out)
    print(f"  Target  : {args.url}", file=out)
    print(f"  Device  : {args.device}", file=out)
    print(f"  Interval: {args.interval}s", file=out)
    print(f"  Timeout : {args.timeout}s", file=out)
    print(f"  Format  : {args.format}", file=out)
    print("  Anomaly injection: ~every 30 readings", file=out)
    print("-" * 48, file=out)

    counter = 0
    t0 = time.time()
    reasoner = EdgeReasoner()

    while True:
        counter += 1
        inject = (counter % 30 == 0)  # inject anomaly every 30 readings
        payload = generate_reading(time.time() - t0, inject_anomaly=inject)
        payload["device_id"] = args.device

        if not args.no_edge:
            reasoner.add(payload["temperature"], payload["humidity"], time.time())
            if args.force_edge_state:
                severity, confidence, code, reason = OVERLAY_STATES[args.force_edge_state]
                payload["edge"] = {
                    "state": args.force_edge_state,
                    "severity": severity,
                    "confidence": confidence,
                    "reason_code": code,
                    "reason": reason,
                }
            else:
                payload["edge"] = reasoner.assess()

        output_payload = (
            to_iotda_property_report(payload, args.service_id)
            if args.format == "iotda"
            else payload
        )

        if args.stdout:
            print(json.dumps(output_payload, ensure_ascii=False))
            ok = True  # JSON-only mode, no HTTP
        elif args.format == "iotda":
            print("  [INFO] IoTDA format is stdout-only until real MQTT credentials are configured.", file=out)
            print(json.dumps(output_payload, ensure_ascii=False), file=out)
            ok = True
        else:
            ok = post_reading(payload, args.url, args.timeout)

        status = "OK" if ok else "FAIL"
        ts = datetime.now().strftime("%H:%M:%S")
        edge = f"  edge={payload['edge']['state']:<16}" if "edge" in payload else ""
        print(
            f"[{ts}] #{counter:04d}  "
            f"temp={payload['temperature']:6.2f}°C  "
            f"hum={payload['humidity']:5.1f}%{edge}  → {status}"
        , file=out)

        if args.count > 0 and counter >= args.count:
            break

        time.sleep(args.interval)


if __name__ == "__main__":
    main()
