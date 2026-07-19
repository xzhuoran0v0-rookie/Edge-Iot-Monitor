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

    while True:
        counter += 1
        inject = (counter % 30 == 0)  # inject anomaly every 30 readings
        payload = generate_reading(time.time() - t0, inject_anomaly=inject)
        payload["device_id"] = args.device
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
        print(
            f"[{ts}] #{counter:04d}  "
            f"temp={payload['temperature']:6.2f}°C  "
            f"hum={payload['humidity']:5.1f}%  "
            f"pres={payload['pressure']:.1f}hPa  → {status}"
        , file=out)

        if args.count > 0 and counter >= args.count:
            break

        time.sleep(args.interval)


if __name__ == "__main__":
    main()
