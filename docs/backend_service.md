# Backend Service

A single C++ binary (`edge_server`) built on cpp-httplib and SQLite. It stores
what the device reports, serves the dashboard, and queues commands.

It does not decide anything. Device verdicts arrive already made, and are stored
and served unchanged.

Source: [`backend/src/`](../backend/src). Entry point: `main.cpp`.

## Responsibilities

1. Accept readings and device verdicts on `POST /api/ingest`.
2. Validate ranges and enforce the device allowlist.
3. Run IQR outlier detection over a sliding window — an independent second
   opinion, stored alongside the device's own.
4. Persist to SQLite: `sensor_readings`, `edge_assessments`, `anomaly_events`,
   `analysis_log`, `device_commands`.
5. Serve the dashboard snapshot on `GET /api/readings`.
6. Queue and hand out device commands.
7. Optionally call an LLM to narrate a state change (see
   [llm_reasoning.md](llm_reasoning.md)).
8. Serve the built frontend as static files.

## Modules

| Module | Role |
|---|---|
| `DataIngestor` | HTTP layer, routing, JSON parse, validation, allowlist |
| `StorageEngine` | SQLite access, all statements prepared and parameterised |
| `DataFilter` | IQR outlier detection over a time window |
| `AIQueryDispatcher` | LLM narration on a background worker thread |
| `ConfigLoader` | `config/config.yaml` into a struct, defaults for missing keys |
| `CloudSync` | **Skeleton only.** No send path implemented. |

## HTTP API

Base URL: `http://<server.host>:<server.port>` (default `0.0.0.0:8080`).
All request and response bodies are JSON.

### `GET /health`

Liveness check.

```json
{"status": "ok"}
```

### `POST /api/ingest`

Device uplink. `temperature` and `humidity` are the only sensor fields
recognised (the hardware is SHT30 only); unknown fields alongside them are
ignored, but a payload with no recognised field is rejected.

```json
{
  "device_id": "esp32s3-001",
  "timestamp": 1234,
  "temperature": 26.5,
  "humidity": 58.0,
  "edge": {
    "state": "NORMAL",
    "severity": "info",
    "confidence": 0.92,
    "reason_code": "STABLE_ENVIRONMENT",
    "reason": "Temperature and humidity are stable."
  }
}
```

`edge` is optional — the simulator and older firmware omit it. If present it
must be an object with a non-empty `state`; a malformed one is a firmware bug
and returns 400 rather than being silently dropped.

Only state *transitions* are stored, so `edge_assessments` is a state timeline
rather than one row per report.

| Code | Meaning |
|---|---|
| 200 | Stored. Body carries `anomaly`, and `analysis` when narration is available. |
| 400 | Malformed JSON, no recognised sensor field, out-of-range value, bad `edge` |
| 401 | `device_id` not in the allowlist |
| 500 | Storage failure |

### `GET /api/readings`

Everything the dashboard needs, in one snapshot.

| Parameter | Required | Default | Notes |
|---|---|---|---|
| `device_id` | yes | — | Must be in the allowlist |
| `limit` | no | 120 | Points per series, 1–1000 |

```json
{
  "status": "ok",
  "device_id": "esp32s3-001",
  "server_time": "2026-08-08T09:55:05Z",
  "online": true,
  "last_seen": "2026-08-08T09:54:57Z",
  "series": {
    "temperature": {"unit": "C", "points": [{"timestamp": "...", "value": 26.5}]},
    "humidity":    {"unit": "%", "points": [{"timestamp": "...", "value": 58.0}]}
  },
  "edge": {
    "state": "NORMAL", "severity": "info", "confidence": 0.92,
    "reason_code": "STABLE_ENVIRONMENT",
    "reason": "Temperature and humidity are stable.",
    "since": "2026-08-08T09:54:57Z"
  }
}
```

- Points are chronological, oldest first — the client does not re-sort.
- `edge.since` is when that state began, not when it was last repeated.
- `edge` is `null` if the device has never reported one.
- `online` is derived from the age of the last reading (30 s = three missed
  10 s reports), **not** from whether the backend is up. A reachable backend
  with a silent device must be visibly different from an unreachable backend.

### `POST /api/prompt`

Free-text question answered against the recent window. Requires an LLM backend
(DeepSeek or Ollama) to be reachable.

```json
{"device_id": "esp32s3-001", "prompt": "Has it been getting warmer?"}
```

### `POST /api/commands`

Queue a command. Requires header `X-Api-Key: <security.command_api_key>` when
that key is configured.

```json
{"device_id": "esp32s3-001", "command": "buzzer_on", "duration_ms": 1000}
```

Allowlisted commands: `buzzer_on`, `buzzer_off`, and `oled:<text>`. Anything
else returns 400 — the device never receives an arbitrary instruction.

`duration_ms` ranges mirror the firmware, because a command the device will
reject must not be queued as if it were accepted:

| Command | Accepted `duration_ms` | Notes |
|---|---|---|
| `buzzer_on` | 0–30000 | 0 becomes 1000 |
| `buzzer_off` | 0 | Forced to 0 |
| `oled:<text>` | 0, or 5000–120000 | 0 is normalised to the device default, 30000 |

`oled:` text must be printable ASCII, at most 240 bytes, with at least one
visible character — the display has no font for anything else and the firmware
rejects it byte by byte.

A queued buzzer command is refused by the device while a local threshold alarm
is active, and acknowledged as `failed`. That is deliberate: the alarm is
decided on the chip, and a network message does not get to switch it off.

Returns 202 with a `command_id` and the normalised `duration_ms`.

### `GET /api/commands/next?device_id=...`

Device poll. Returns the oldest pending command, or `{"status": "idle"}`.

### `POST /api/commands/ack`

```json
{"device_id": "esp32s3-001", "command_id": 7, "result": "done"}
```

`result` must be `done` or `failed`.

## Static files

The dashboard is served from `frontend/dist`. The server tries `frontend/dist`,
`../frontend/dist`, and `../../frontend/dist`, so it works whether it is started
from the repository root or from the build directory, and logs which one it
mounted.

## Database

Schema: [`sql/schema.sql`](../sql/schema.sql), executed by `StorageEngine::init()`
so there is one source of truth. Current `schema_version` is 3.

| Table | Contents |
|---|---|
| `sensor_readings` | One row per quantity per report |
| `edge_assessments` | Device verdict transitions (state, severity, confidence, reason, start time) |
| `anomaly_events` | Server-side IQR detections |
| `analysis_log` | LLM prompts and responses |
| `device_commands` | Command queue with status and ack |
| `sync_status` | Reserved for `CloudSync` |

## Configuration

Copy `config/config.example.yaml` to `config/config.yaml`. The file is
gitignored; it may hold an API key and must never be committed — or included in
a submission archive.

Notable keys:

| Key | Purpose |
|---|---|
| `server.host` / `server.port` | Listen address |
| `sqlite.db_path` | Database file |
| `devices.allowlist` | Accepted `device_id`s; empty accepts all |
| `security.command_api_key` | Required for `POST /api/commands` when set |
| `validation.*` | Accepted reading ranges |
| `ai.enabled` | Master switch for LLM narration |
| `deepseek.*` / `ollama.*` | Narration backends |
| `cloud.*` | Reserved for `CloudSync` |

`DEEPSEEK_API_KEY` in the environment takes priority over the file.

## Not implemented

`CloudSync` counts pending records and logs them. There is no send path, and no
backend → IoTDA command downlink. The device's own MQTT publish to IoTDA is
separate and does work; this gap is specifically the *server* forwarding
direction.

## Build and test

```bash
cmake -S . -B build-cmake -DEDGE_BUILD_SERVER=ON
cmake --build build-cmake
./build-cmake/backend/edge_server
```

```bash
python3 scripts/test_backend_api.py
```

The smoke tests cover the allowlist, range validation, the edge-assessment round
trip (including transition-only storage), command auth and the allowlist, and
the narration trigger.
