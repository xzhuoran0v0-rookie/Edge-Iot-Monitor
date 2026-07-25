#!/usr/bin/env python3
"""
End-to-end smoke tests for the local C++ backend API.

The script starts edge_server with a temporary config/database, exercises ingest
and command endpoints, then shuts the server down.
"""

import json
import argparse
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build-cmake" / "backend" / "edge_server"


def write_temp_project(tmp: Path) -> None:
    (tmp / "config").mkdir()
    (tmp / "sql").mkdir()
    shutil.copy(ROOT / "sql" / "schema.sql", tmp / "sql" / "schema.sql")
    (tmp / "config" / "config.yaml").write_text(
        f"""
server:
  host: "127.0.0.1"
  port: 18080
  max_connections: 4
  request_timeout_ms: 5000
sqlite:
  db_path: "{tmp / 'sensor.db'}"
ollama:
  host: "http://127.0.0.1"
  port: 11434
  model: "qwen2.5:3b"
  timeout_s: 1
  max_tokens: 64
  temperature: 0.1
filter:
  window_seconds: 60
  iqr_multiplier: 1.5
  min_window_samples: 10
ai:
  enabled: false
  trigger_every_n_records: 2
  max_window_records: 20
devices:
  allowlist:
    - "esp32s3-001"
security:
  command_api_key: "test-key"
validation:
  temperature:
    min: -40.0
    max: 85.0
  humidity:
    min: 0.0
    max: 100.0
cloud:
  enabled: false
""".strip()
        + "\n",
        encoding="utf-8",
    )


def write_narration_project(tmp: Path) -> None:
    """Config for the narration-trigger test.

    Both LLM backends point at a dead port on purpose: the call always fails,
    but the server still logs one "Narrating ... reason:" line per firing, which
    is what the trigger assertions read. No model or API key is needed.
    """
    (tmp / "config").mkdir()
    (tmp / "sql").mkdir()
    shutil.copy(ROOT / "sql" / "schema.sql", tmp / "sql" / "schema.sql")
    (tmp / "config" / "config.yaml").write_text(
        f"""
server:
  host: "127.0.0.1"
  port: 18081
sqlite:
  db_path: "{tmp / 'sensor.db'}"
ollama:
  host: "http://127.0.0.1"
  port: 1
  timeout_s: 1
deepseek:
  enabled: false
filter:
  window_seconds: 60
  iqr_multiplier: 1.5
  min_window_samples: 10
ai:
  enabled: true
  max_window_records: 40
  trigger:
    min_interval_s: 30
    temp_delta_c: 2.0
    humidity_delta: 5.0
    warn_temp_c: 35.0
    warn_humidity: 80.0
devices:
  allowlist:
    - "esp32s3-001"
cloud:
  enabled: false
""".strip()
        + "\n",
        encoding="utf-8",
    )


def request(method: str, path: str, payload=None, headers=None, port: int = 18080):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=data,
        method=method,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))


def request_text(path: str, port: int = 18080):
    """Fetch a non-JSON response (the status page) as text."""
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method="GET")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8")


def assert_response(actual, expected_status: int, expected_status_field: str | None = None):
    status, body = actual
    if status != expected_status:
        raise AssertionError(f"expected HTTP {expected_status}, got {status}: {body}")
    if expected_status_field is not None and body.get("status") != expected_status_field:
        raise AssertionError(f"expected status={expected_status_field}, got {body}")
    return body


def wait_for_health(proc: subprocess.Popen, timeout_s: float = 5.0, port: int = 18080) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with {proc.returncode}")
        try:
            assert_response(request("GET", "/health", port=port), 200, "ok")
            return
        except Exception:
            time.sleep(0.1)
    raise TimeoutError("server did not become healthy")


def run_narration_trigger_test(binary: Path) -> None:
    """The LLM narrates on state CHANGE, never on a record count.

    A count-based trigger re-analysed identical steady-state data forever. This
    asserts the replacement: silent while nothing changes, one narration when
    something does.

    Scripted so the count is deterministic:
      1 baseline  + 12 identical readings (silent)
      + 1 reading crossing the 35 C warn band (crossing bypasses the cooldown)
      + 8 identical readings at the new level (silent: crossing is latched, and
        the IQR anomalies it provokes are held down by the 30 s cooldown)
      => exactly 2 narrations from 22 ingests.
    """
    with tempfile.TemporaryDirectory(prefix="edge-iot-narration-") as tmp_dir:
        tmp = Path(tmp_dir)
        write_narration_project(tmp)
        proc = subprocess.Popen(
            [str(binary)],
            cwd=tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_health(proc, port=18081)

            def send(temp: float) -> None:
                assert_response(
                    request(
                        "POST",
                        "/api/ingest",
                        {"device_id": "esp32s3-001", "temperature": temp, "humidity": 50.0},
                        port=18081,
                    ),
                    200,
                    "ok",
                )

            send(22.0)                      # baseline -> narrates
            for _ in range(12):             # identical readings -> silent
                send(22.0)
            send(36.0)                      # crosses 35 C -> narrates
            for _ in range(8):              # identical at new level -> silent
                send(36.0)
            time.sleep(1.0)                 # let the worker drain
        finally:
            proc.send_signal(signal.SIGINT)
            try:
                out, _ = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate(timeout=5)

    firings = [ln for ln in out.splitlines() if "Narrating" in ln]
    if len(firings) != 2:
        raise AssertionError(
            f"expected exactly 2 narrations from 22 ingests, got {len(firings)}:\n"
            + "\n".join(firings)
        )
    if "first report" not in firings[0]:
        raise AssertionError(f"first narration should be the baseline, got: {firings[0]}")
    if "rose above" not in firings[1]:
        raise AssertionError(f"second narration should be the band crossing, got: {firings[1]}")


def parse_args():
    parser = argparse.ArgumentParser(description="Run backend HTTP API smoke tests.")
    parser.add_argument(
        "--binary",
        default=str(BINARY),
        help=f"Path to edge_server binary (default: {BINARY})",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    binary = Path(args.binary)

    if not binary.exists():
        print(f"Missing backend binary: {binary}", file=sys.stderr)
        print("Run: cmake -S . -B build-cmake -DEDGE_BUILD_SERVER=ON && cmake --build build-cmake", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="edge-iot-api-") as tmp_dir:
        tmp = Path(tmp_dir)
        write_temp_project(tmp)
        proc = subprocess.Popen(
            [str(binary)],
            cwd=tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_health(proc)

            assert_response(
                request("POST", "/api/ingest", {"device_id": "bad", "temperature": 24, "humidity": 50}),
                401,
                "error",
            )
            # The hardware is SHT30 only — there is no pressure sensor. A payload
            # carrying nothing but pressure has no recognised field and is
            # rejected rather than stored under a sensor type nothing produces.
            assert_response(
                request("POST", "/api/ingest", {"device_id": "esp32s3-001", "pressure": 1013.2}),
                400,
                "error",
            )
            # Unrecognised fields alongside real ones are ignored, not fatal:
            # a future firmware may send extra keys this backend does not know.
            assert_response(
                request(
                    "POST",
                    "/api/ingest",
                    {"device_id": "esp32s3-001", "temperature": 24.2, "humidity": 50, "pressure": 1013.2},
                ),
                200,
                "ok",
            )
            # ---- read-only status endpoint ----
            assert_response(request("GET", "/api/status"), 400, "error")
            assert_response(request("GET", "/api/status?device_id=bad"), 401, "error")

            status_body = assert_response(
                request("GET", "/api/status?device_id=esp32s3-001"), 200, "ok"
            )
            latest = {r["sensor_type"]: r for r in status_body["readings"]}
            # One row per sensor_type, not the raw recent-N window.
            if sorted(latest) != ["humidity", "temperature"]:
                raise AssertionError(f"expected one row per sensor type, got {status_body['readings']}")
            if latest["temperature"]["value"] != 24.2 or latest["temperature"]["unit"] != "C":
                raise AssertionError(f"unexpected temperature row: {latest['temperature']}")
            # ai.enabled is false in this config, so nothing should have narrated.
            if status_body["narration"] is not None:
                raise AssertionError(f"expected no narration, got {status_body['narration']}")

            page_status, page = request_text("/")
            if page_status != 200 or "Edge IoT Monitor" not in page:
                raise AssertionError(f"status page did not render (HTTP {page_status})")
            # Self-contained: no CDN, no external stylesheet, nothing to fetch.
            for external in ("http://", "https://", "<link", "src="):
                if external in page:
                    raise AssertionError(f"status page references external asset: {external}")

            assert_response(
                request(
                    "POST",
                    "/api/commands",
                    {"device_id": "esp32s3-001", "command": "buzzer_on", "duration_ms": 100},
                ),
                401,
                "error",
            )
            queued = assert_response(
                request(
                    "POST",
                    "/api/commands",
                    {"device_id": "esp32s3-001", "command": "buzzer_on", "duration_ms": 100},
                    {"X-Api-Key": "test-key"},
                ),
                202,
                "queued",
            )
            command_id = queued["command_id"]
            next_path = "/api/commands/next?device_id=" + urllib.parse.quote("esp32s3-001")
            next_body = assert_response(request("GET", next_path), 200, "ok")
            if next_body["command_id"] != command_id:
                raise AssertionError(f"expected command_id {command_id}, got {next_body}")
            assert_response(
                request(
                    "POST",
                    "/api/commands/ack",
                    {"device_id": "esp32s3-001", "command_id": command_id, "result": "done"},
                ),
                200,
                "acked",
            )
            assert_response(
                request(
                    "POST",
                    "/api/commands",
                    {"device_id": "esp32s3-001", "command": "gpio_write", "duration_ms": 1},
                    {"X-Api-Key": "test-key"},
                ),
                400,
                "error",
            )

            print("backend API smoke tests passed")
        finally:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)

    run_narration_trigger_test(binary)
    print("narration trigger tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
