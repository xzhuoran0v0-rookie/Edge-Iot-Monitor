-- ============================================================
-- Edge-IoT Monitor — SQLite 3 Schema (StorageEngine-compatible)
-- ============================================================
-- This schema matches `backend/src/StorageEngine.cpp` (tables + column names).
--
-- Data model:
-- - Each incoming JSON reading is split into multiple rows (temperature/humidity/pressure)
-- - Stored as: (device_id, sensor_type, value, unit, timestamp)
--
-- Use `scripts/simulate_sensor.py --stdout` + `backend/src/ingest_stdin.cpp`
-- to test inserts quickly without an HTTP server.

PRAGMA foreign_keys = ON;

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

-- Cloud sync progress tracker (used by CloudSync)
-- One row per table, stores the last synced row id.
CREATE TABLE IF NOT EXISTS sync_status (
    table_name      TEXT PRIMARY KEY,
    last_synced_id  INTEGER NOT NULL DEFAULT 0
);
