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
  pressure:
    min: 800.0
    max: 1200.0
cloud:
  enabled: false
""".strip()
        + "\n",
        encoding="utf-8",
    )


def request(method: str, path: str, payload=None, headers=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"http://127.0.0.1:18080{path}",
        data=data,
        method=method,
        headers={"Content-Type": "application/json", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))


def assert_response(actual, expected_status: int, expected_status_field: str | None = None):
    status, body = actual
    if status != expected_status:
        raise AssertionError(f"expected HTTP {expected_status}, got {status}: {body}")
    if expected_status_field is not None and body.get("status") != expected_status_field:
        raise AssertionError(f"expected status={expected_status_field}, got {body}")
    return body


def wait_for_health(proc: subprocess.Popen, timeout_s: float = 5.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with {proc.returncode}")
        try:
            assert_response(request("GET", "/health"), 200, "ok")
            return
        except Exception:
            time.sleep(0.1)
    raise TimeoutError("server did not become healthy")


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
            assert_response(
                request("POST", "/api/ingest", {"device_id": "esp32s3-001", "pressure": 1013.2}),
                200,
                "ok",
            )
            assert_response(
                request(
                    "POST",
                    "/api/ingest",
                    {"device_id": "esp32s3-001", "temperature": 24.2, "humidity": 50, "pressure": 1013.2},
                ),
                200,
                "ok",
            )
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
            return 0
        finally:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
