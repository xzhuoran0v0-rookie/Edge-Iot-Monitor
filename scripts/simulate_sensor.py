#!/usr/bin/env python3
"""
simulate_sensor.py — Sensor Data Simulator
===========================================
Generates realistic sensor readings and POSTs them to the C++ backend,
allowing full backend + AI pipeline testing WITHOUT any physical hardware.

Usage:
    python3 scripts/simulate_sensor.py [--interval 2] [--device esp32-s3-001]
"""

import argparse
import json
import math
import random
import time
import urllib.request
import urllib.error
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# Config (override via CLI args)
# ---------------------------------------------------------------------------
BACKEND_URL  = "http://127.0.0.1:8080/api/ingest"
DEVICE_ID    = "esp32-s3-sim-001"
INTERVAL_SEC = 2.0   # seconds between readings

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
    pressure    = round(1013.0    + random.gauss(0, 0.2), 2)

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
        "pressure":    pressure,
    }


def post_reading(payload: dict) -> bool:
    data = json.dumps(payload).encode("utf-8")
    req  = urllib.request.Request(
        BACKEND_URL,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
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
    args = parser.parse_args()

    print(f"Simulator started")
    print(f"  Target  : {args.url}")
    print(f"  Device  : {args.device}")
    print(f"  Interval: {args.interval}s")
    print(f"  Anomaly injection: ~every 30 readings")
    print("-" * 48)

    counter = 0
    t0 = time.time()

    while True:
        counter += 1
        inject = (counter % 30 == 0)  # inject anomaly every 30 readings
        payload = generate_reading(time.time() - t0, inject_anomaly=inject)
        payload["device_id"] = args.device

        ok = post_reading(payload)
        status = "OK" if ok else "FAIL"
        ts = datetime.now().strftime("%H:%M:%S")
        print(
            f"[{ts}] #{counter:04d}  "
            f"temp={payload['temperature']:6.2f}°C  "
            f"hum={payload['humidity']:5.1f}%  "
            f"pres={payload['pressure']:.1f}hPa  → {status}"
        )

        time.sleep(args.interval)


if __name__ == "__main__":
    main()
