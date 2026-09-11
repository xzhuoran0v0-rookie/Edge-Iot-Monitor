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


def run_edge_assessment_test() -> None:
    """The device's own verdict must survive the trip to the dashboard.

    EdgeReasoner runs on the ESP32 and ships its conclusion in the ingest
    payload. Before this path existed the backend parsed only temperature and
    humidity, so the headline feature was silently dropped at the door. This
    asserts the round trip: ingest -> store -> GET /api/readings.
    """
    def ingest(temp: float, hum: float, edge=None):
        payload = {"device_id": "esp32s3-001", "temperature": temp, "humidity": hum}
        if edge is not None:
            payload["edge"] = edge
        return request("POST", "/api/ingest", payload)

    def assessment(state: str, severity: str, code: str) -> dict:
        return {
            "state": state,
            "severity": severity,
            "confidence": 0.87,
            "reason_code": code,
            "reason": "Temperature and humidity are both elevated.",
        }

    readings_path = "/api/readings?device_id=" + urllib.parse.quote("esp32s3-001")

    # A payload with no "edge" object stays valid: the simulator and older
    # firmware do not send one.
    assert_response(ingest(24.0, 50.0), 200, "ok")

    assert_response(
        ingest(31.0, 72.0, assessment("HEAT_HUMID_RISK", "warning", "HOT_AND_HUMID")),
        200,
        "ok",
    )

    body = assert_response(request("GET", readings_path), 200, "ok")
    edge = body.get("edge")
    if not edge or edge["state"] != "HEAT_HUMID_RISK":
        raise AssertionError(f"edge assessment did not reach /api/readings: {body}")
    if edge["severity"] != "warning" or edge["reason_code"] != "HOT_AND_HUMID":
        raise AssertionError(f"edge assessment altered in transit: {edge}")
    if not edge.get("since"):
        raise AssertionError(f"edge assessment has no start time: {edge}")

    # Readings come back grouped, oldest first, so the dashboard can plot them
    # without re-sorting.
    temps = body["series"]["temperature"]["points"]
    if len(temps) < 2 or temps[0]["timestamp"] > temps[-1]["timestamp"]:
        raise AssertionError(f"temperature series is not chronological: {temps}")

    # The device repeats its verdict every cycle; only transitions are recorded,
    # so "since" keeps pointing at when the state actually began.
    first_since = edge["since"]
    time.sleep(1.1)  # timestamps have 1 s resolution
    for _ in range(3):
        assert_response(
            ingest(31.5, 73.0, assessment("HEAT_HUMID_RISK", "warning", "HOT_AND_HUMID")),
            200,
            "ok",
        )
    repeated = assert_response(request("GET", readings_path), 200, "ok")["edge"]
    if repeated["since"] != first_since:
        raise AssertionError(
            f"unchanged state should keep its original start time: {first_since} -> {repeated['since']}"
        )

    # A real transition moves it.
    assert_response(
        ingest(24.0, 50.0, assessment("NORMAL", "info", "STABLE_ENVIRONMENT")),
        200,
        "ok",
    )
    changed = assert_response(request("GET", readings_path), 200, "ok")["edge"]
    if changed["state"] != "NORMAL" or changed["since"] == first_since:
        raise AssertionError(f"state change was not recorded: {changed}")

    # A malformed edge object is a firmware bug — fail loudly instead of
    # accepting the reading and quietly showing no reasoning on the dashboard.
    assert_response(ingest(24.0, 50.0, "not-an-object"), 400, "error")

    # Unknown devices cannot read another device's data.
    assert_response(
        request("GET", "/api/readings?device_id=" + urllib.parse.quote("bad")),
        401,
        "error",
    )
    assert_response(request("GET", "/api/readings"), 400, "error")


def run_oled_command_test() -> None:
    """The backend must not queue a command the device will reject.

    The firmware's showCustomMessage() accepts 5000-120000 ms and printable
    ASCII only. The backend used to accept 0-30000 ms and any text, so an
    oled: command with duration 1000, or with Chinese text, returned 202 and
    then failed silently on the device.
    """
    key = {"X-Api-Key": "test-key"}

    def create(command: str, duration_ms: int):
        return request(
            "POST",
            "/api/commands",
            {"device_id": "esp32s3-001", "command": command, "duration_ms": duration_ms},
            key,
        )

    # Below the firmware minimum: the device would reject it, so we must too.
    assert_response(create("oled:HELLO", 1000), 400, "error")
    # Above the firmware maximum.
    assert_response(create("oled:HELLO", 200000), 400, "error")
    # In range.
    assert_response(create("oled:HELLO", 8000), 202, "queued")

    # 0 means "device default" and is stored explicitly, so the queued row
    # states what will actually happen instead of implying zero duration.
    queued = assert_response(create("oled:ALL CLEAR", 0), 202, "queued")
    if queued["duration_ms"] != 30000:
        raise AssertionError(f"expected duration_ms normalised to 30000, got {queued}")

    # The firmware rejects non-ASCII text byte by byte.
    assert_response(create("oled:café", 8000), 400, "error")
    assert_response(create("oled:   ", 8000), 400, "error")
    assert_response(create("oled:", 8000), 400, "error")

    # The buzzer keeps its own range, which is different from the OLED's.
    assert_response(create("buzzer_on", 1000), 202, "queued")
    assert_response(create("buzzer_on", 60000), 400, "error")


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

            run_edge_assessment_test()
            run_oled_command_test()

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
