#!/usr/bin/env python3
"""
test_ollama.py
==============
Verifies the full local AI inference pipeline:
  1. Checks that Ollama is running and reachable
  2. Checks that the Qwen2.5-3B model is available
  3. Sends a synthetic sensor data window prompt (exactly as AIQueryDispatcher would)
  4. Displays the model's response, token counts, and inference time
  5. Exits 0 on success, 1 on any failure

Usage:
    python3 scripts/test_ollama.py [options]

Examples:
    # Default: localhost:11434
    python3 scripts/test_ollama.py

    # Remote Ollama instance
    python3 scripts/test_ollama.py --host 192.168.1.10 --port 11434

    # Quick connectivity check only (no inference)
    python3 scripts/test_ollama.py --check-only

    # Use a different model tag
    python3 scripts/test_ollama.py --model qwen2.5:3b
"""

import argparse
import json
import sys
import time
from datetime import datetime, timezone, timedelta
import random
import math

try:
    import requests
except ImportError:
    print("ERROR: 'requests' library not found. Install with: pip3 install requests")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Configuration defaults
# ---------------------------------------------------------------------------
DEFAULT_HOST = "http://127.0.0.1"
DEFAULT_PORT = 11434
DEFAULT_MODEL = "qwen2.5:3b"
DEFAULT_TIMEOUT = 180  # seconds — CPU inference can take 30–60 s


# ---------------------------------------------------------------------------
# Step 1: Check Ollama health
# ---------------------------------------------------------------------------

def check_ollama_health(base_url: str, timeout: float = 5.0) -> bool:
    """GET /api/tags — returns True if Ollama responds."""
    try:
        r = requests.get(f"{base_url}/api/tags", timeout=timeout)
        r.raise_for_status()
        return True
    except requests.exceptions.ConnectionError:
        return False
    except requests.exceptions.HTTPError as e:
        print(f"  Ollama returned HTTP error: {e}")
        return False


# ---------------------------------------------------------------------------
# Step 2: Check model availability
# ---------------------------------------------------------------------------

def get_available_models(base_url: str, timeout: float = 5.0) -> list[str]:
    """Return list of model tags available in Ollama."""
    try:
        r = requests.get(f"{base_url}/api/tags", timeout=timeout)
        r.raise_for_status()
        data = r.json()
        return [m["name"] for m in data.get("models", [])]
    except Exception as e:
        print(f"  Could not list models: {e}")
        return []


def model_is_available(base_url: str, model_name: str, timeout: float = 5.0) -> bool:
    """Return True if model_name is in the list of pulled models."""
    models = get_available_models(base_url, timeout)
    # Exact match or prefix match (e.g. "qwen2.5:3b" matches "qwen2.5:3b:latest")
    for m in models:
        if m == model_name or m.startswith(model_name):
            return True
    return False


# ---------------------------------------------------------------------------
# Step 3: Build synthetic sensor window (mimics AIQueryDispatcher prompt)
# ---------------------------------------------------------------------------

def build_synthetic_sensor_window(
    device_id: str = "esp32s3-001",
    n_records: int = 20,
    n_anomalies: int = 2,
) -> dict:
    """
    Generate a synthetic sensor data window.
    Returns a dict with timestamps, temperatures, humidities, and anomaly count.
    """
    now = datetime.now(timezone.utc)
    records = []

    base_temp = 23.0
    base_hum = 57.0

    for i in range(n_records):
        t = now - timedelta(seconds=(n_records - i) * 10)
        phase = (2 * math.pi * i) / n_records
        temp = base_temp + 1.5 * math.sin(phase) + random.gauss(0, 0.2)
        hum = base_hum + 3.0 * math.cos(phase) + random.gauss(0, 0.8)
        records.append({
            "ts": t.strftime("%H:%M:%S"),
            "temp": round(temp, 2),
            "hum": round(hum, 1),
        })

    # Inject anomalies
    for _ in range(n_anomalies):
        idx = random.randint(0, n_records - 1)
        if random.random() > 0.5:
            records[idx]["temp"] += random.uniform(8.0, 12.0)
        else:
            records[idx]["hum"] += random.uniform(15.0, 25.0)

    # Flatten into per-metric rows, matching C++ SensorReading model
    readings = []
    for r in records:
        readings.append({"sensor_type": "temperature", "value": r["temp"], "unit": "C", "timestamp": r["ts"]})
        readings.append({"sensor_type": "humidity",    "value": r["hum"],  "unit": "%", "timestamp": r["ts"]})

    return {
        "device_id": device_id,
        "readings": readings,
        "anomaly_count": n_anomalies,
        "window_start": records[0]["ts"],
        "window_end":  records[-1]["ts"],
    }


def build_prompt(window: dict) -> str:
    """
    Build the exact prompt that AIQueryDispatcher sends to Ollama.
    Mirrors AIQueryDispatcher.cpp — keep the two in sync.
    """
    lines = []
    lines.append("You are an IoT sensor data analyst.")
    lines.append(f"Analyze the recent readings from device [{window['device_id']}] and provide insights.")
    lines.append("")
    lines.append("## Sensor Readings")
    for r in window["readings"]:
        lines.append(f"{r['timestamp']}  {r['sensor_type']}  {r['value']} {r['unit']}")
    lines.append("")
    lines.append("Answer in 2-3 sentences:")
    lines.append("1. Is the system stable?")
    lines.append("2. Any anomaly or trend?")
    lines.append("3. Suggestions?")
    lines.append("Be concise.")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Step 4: Call Ollama inference
# ---------------------------------------------------------------------------

def call_ollama(
    base_url: str,
    model: str,
    prompt: str,
    timeout: float = DEFAULT_TIMEOUT,
) -> dict:
    """
    POST to /api/generate with stream=False.
    Returns the full Ollama response dict.
    """
    payload = {
        "model": model,
        "prompt": prompt,
        "stream": False,
        "options": {
            "temperature": 0.3,
            "num_predict": 512,
        },
    }

    response = requests.post(
        f"{base_url}/api/generate",
        json=payload,
        timeout=timeout,
    )
    response.raise_for_status()
    return response.json()


# ---------------------------------------------------------------------------
# CLI argument parsing
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description="Verify Ollama + Qwen2 AI inference pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"Ollama host (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Ollama port (default: {DEFAULT_PORT})")
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help=f"Model tag (default: {DEFAULT_MODEL})")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"Inference timeout in seconds (default: {DEFAULT_TIMEOUT})")
    parser.add_argument("--check-only", action="store_true",
                        help="Only check connectivity and model availability, skip inference")
    parser.add_argument("--records", type=int, default=20,
                        help="Number of synthetic sensor records in the test prompt (default: 20)")
    parser.add_argument("--anomalies", type=int, default=2,
                        help="Number of injected anomalies in the test window (default: 2)")
    return parser.parse_args()

# ---------------------------------------------------------------------------
# Main test runner
# ---------------------------------------------------------------------------

def main():
    args = parse_args()
    base_url = f"{args.host}:{args.port}"
    all_passed = True

    print()
    print("=" * 62)
    print("  Ollama + Qwen2.5 Pipeline Verification")
    print("=" * 62)
    print(f"  Target : {base_url}")
    print(f"  Model  : {args.model}")
    print("=" * 62)
    print()

    # ------------------------------------------------------------------
    # Step 1: Health check
    # ------------------------------------------------------------------
    print("[ Step 1 ]  Checking Ollama connectivity...")
    if check_ollama_health(base_url):
        print("  ✓ Ollama is reachable")
    else:
        print("  ✗ Ollama is NOT reachable")
        print()
        print("  Troubleshooting:")
        print("    1. Is Ollama installed?  curl -fsSL https://ollama.com/install.sh | sh")
        print("    2. Is Ollama running?    ollama serve")
        print(f"    3. Is it on port {args.port}?  lsof -i :{args.port}")
        sys.exit(1)

    # ------------------------------------------------------------------
    # Step 2: Model availability
    # ------------------------------------------------------------------
    print()
    print("[ Step 2 ]  Checking model availability...")
    available = get_available_models(base_url)

    if available:
        print(f"  Available models ({len(available)}):")
        for m in available:
            marker = "  ✓" if (m == args.model or m.startswith(args.model)) else "   "
            print(f"  {marker}  {m}")
    else:
        print("  No models found.")

    if model_is_available(base_url, args.model):
        print(f"  ✓ Model '{args.model}' is ready")
    else:
        print(f"  ✗ Model '{args.model}' is NOT available")
        print()
        print("  Pull it with:")
        print(f"    ollama pull {args.model}")
        all_passed = False
        if args.check_only:
            sys.exit(1)

    if args.check_only:
        print()
        print("  --check-only mode: skipping inference. All checks passed.")
        sys.exit(0)

    if not all_passed:
        print()
        print("  Cannot run inference — model not available. Exiting.")
        sys.exit(1)

    # ------------------------------------------------------------------
    # Step 3: Build prompt
    # ------------------------------------------------------------------
    print()
    print("[ Step 3 ]  Building synthetic sensor data window...")
    window = build_synthetic_sensor_window(
        device_id="esp32s3-001",
        n_records=args.records,
        n_anomalies=args.anomalies,
    )
    prompt = build_prompt(window)
    print(f"  Window   : {window['window_start']} – {window['window_end']} UTC")
    print(f"  Readings : {len(window['readings'])} rows ({args.records} samples)")
    print(f"  Anomalies: {args.anomalies} injected")
    print()
    print("  Prompt preview (first 400 chars):")
    print("  " + "-" * 56)
    for line in prompt[:400].splitlines():
        print(f"  {line}")
    if len(prompt) > 400:
        print("  ...")
    print("  " + "-" * 56)

    # ------------------------------------------------------------------
    # Step 4: Inference
    # ------------------------------------------------------------------
    print()
    print(f"[ Step 4 ]  Running inference (timeout: {args.timeout:.0f} s)...")
    print("  (This may take 10–60 seconds on CPU. Please wait.)")
    print()

    t_start = time.time()
    try:
        result = call_ollama(base_url, args.model, prompt, timeout=args.timeout)
        elapsed = time.time() - t_start
    except requests.exceptions.Timeout:
        print(f"  ✗ Inference timed out after {args.timeout:.0f} seconds")
        print("    Try increasing --timeout or check system load")
        sys.exit(1)
    except requests.exceptions.HTTPError as e:
        print(f"  ✗ HTTP error during inference: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"  ✗ Unexpected error: {e}")
        sys.exit(1)

    # ------------------------------------------------------------------
    # Step 5: Display results
    # ------------------------------------------------------------------
    analysis = result.get("response", "").strip()
    prompt_tokens = result.get("prompt_eval_count", "N/A")
    response_tokens = result.get("eval_count", "N/A")
    done = result.get("done", False)

    print("=" * 62)
    print("  AI ANALYSIS RESULT")
    print("=" * 62)
    print(analysis)
    print()
    print("=" * 62)
    print("  METADATA")
    print("=" * 62)
    print(f"  Model              : {args.model}")
    print(f"  Done flag          : {done}")
    print(f"  Prompt tokens      : {prompt_tokens}")
    print(f"  Response tokens    : {response_tokens}")
    print(f"  Wall-clock time    : {elapsed:.2f} s")
    if isinstance(response_tokens, int) and elapsed > 0:
        tok_per_s = response_tokens / elapsed
        print(f"  Tokens/second      : {tok_per_s:.1f}")
    print("=" * 62)
    print()

    if done and analysis:
        print("  ✓ AI pipeline verified successfully!")
        print()
        print("  Next steps:")
        print("    1. Start the C++ backend:  ./build/edge_backend")
        print("    2. Run the simulator:      python3 scripts/simulate_sensor.py")
        print("    3. Check SQLite results:")
        print("         sqlite3 sensor.db \"SELECT COUNT(*) FROM sensor_readings;\"")
        print("         sqlite3 sensor.db \"SELECT response FROM analysis_log ORDER BY id DESC LIMIT 1;\"")
        sys.exit(0)
    else:
        print("  ✗ Inference completed but response appears empty or incomplete.")
        print("    Raw result:")
        print(json.dumps(result, indent=2))
        sys.exit(1)


if __name__ == "__main__":
    main()
