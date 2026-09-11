-- ============================================================
-- Edge-IoT Monitor — SQLite 3 Schema (StorageEngine-compatible)
-- ============================================================
-- This schema matches `backend/src/StorageEngine.cpp` (tables + column names).
--
-- Data model:
-- - Each incoming JSON reading is split into one row per quantity (temperature/humidity)
-- - Stored as: (device_id, sensor_type, value, unit, timestamp)
-- - sensor_type is a free-form string, so adding a sensor needs no schema change
--
-- Use `scripts/simulate_sensor.py --stdout` + `backend/src/ingest_stdin.cpp`
-- to test inserts quickly without an HTTP server.

PRAGMA foreign_keys = ON;

-- Lightweight schema version marker. This is not a full migration runner yet,
-- but it gives future migrations an authoritative checkpoint.
CREATE TABLE IF NOT EXISTS schema_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

INSERT OR REPLACE INTO schema_meta (key, value)
VALUES ('schema_version', '3');

-- Raw sensor datapoints (one metric per row)
CREATE TABLE IF NOT EXISTS sensor_readings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    sensor_type TEXT NOT NULL,
    value       REAL NOT NULL,
    unit        TEXT,
    timestamp   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_sensor_readings_device_time
    ON sensor_readings (device_id, timestamp);

-- Anomaly detection events (placeholder for DataFilter)
CREATE TABLE IF NOT EXISTS anomaly_events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    sensor_type TEXT NOT NULL,
    value       REAL NOT NULL,
    timestamp   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_anomaly_events_device_time
    ON anomaly_events (device_id, timestamp);

-- On-device reasoning results.
--
-- The ESP32 runs EdgeReasoner locally and ships its verdict alongside the raw
-- readings (the "edge" object in the ingest payload). This table is where that
-- verdict lands: it is the device's own conclusion, not a server-side one, so
-- it is kept apart from analysis_log (LLM narration) and anomaly_events (IQR).
CREATE TABLE IF NOT EXISTS edge_assessments (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    state       TEXT NOT NULL,
    severity    TEXT NOT NULL,
    confidence  REAL,
    reason_code TEXT,
    reason      TEXT,
    timestamp   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_edge_assessments_device_time
    ON edge_assessments (device_id, timestamp);

-- AI analysis logs (placeholder for AIQueryDispatcher/Ollama)
CREATE TABLE IF NOT EXISTS analysis_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    prompt      TEXT,
    response    TEXT,
    timestamp   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_analysis_log_device_time
    ON analysis_log (device_id, timestamp);

-- Device commands queued by local backend / future cloud command bridge.
-- LLM output must be translated into this small allowlisted command set before
-- the ESP32 ever sees it.
CREATE TABLE IF NOT EXISTS device_commands (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    command     TEXT NOT NULL,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    status      TEXT NOT NULL DEFAULT 'pending',
    created_at  TEXT NOT NULL,
    acked_at    TEXT,
    result      TEXT
);

CREATE INDEX IF NOT EXISTS idx_device_commands_device_status
    ON device_commands (device_id, status, id);
