-- ============================================================
-- Edge-Intelligence IoT Monitor — MySQL 8.0 Schema
-- ============================================================
-- Run with: mysql -u root -p < sql/schema.sql
--
-- Creates:
--   Database : sensor_db
--   User     : iot_user  (with limited privileges)
--   Tables   : sensor_readings, anomaly_events, analysis_log
-- ============================================================

-- ------------------------------------------------------------
-- Database
-- ------------------------------------------------------------
CREATE DATABASE IF NOT EXISTS sensor_db
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE sensor_db;

-- ------------------------------------------------------------
-- Application user (adjust password before running in production)
-- ------------------------------------------------------------
CREATE USER IF NOT EXISTS 'iot_user'@'localhost' IDENTIFIED BY 'CHANGE_ME';
GRANT SELECT, INSERT, UPDATE, DELETE, INDEX
    ON sensor_db.*
    TO 'iot_user'@'localhost';
FLUSH PRIVILEGES;

-- ============================================================
-- Table 1: sensor_readings
-- Stores every validated reading received from ESP32-S3 devices.
-- The is_anomaly flag is set by DataFilter after IQR analysis.
-- ============================================================
CREATE TABLE IF NOT EXISTS sensor_readings (
    -- Primary key
    id              BIGINT UNSIGNED     NOT NULL AUTO_INCREMENT,

    -- Device identification
    device_id       VARCHAR(64)         NOT NULL
                    COMMENT 'Unique identifier of the ESP32-S3 device (e.g. esp32s3-001)',

    -- Sensor timestamp (from ESP32 onboard clock / NTP)
    -- Stored with millisecond precision for high-frequency logging
    reading_time    DATETIME(3)         NOT NULL
                    COMMENT 'Timestamp of the sensor reading (UTC, millisecond precision)',

    -- Environmental measurements
    temperature     FLOAT               NOT NULL
                    COMMENT 'Temperature in degrees Celsius',

    humidity        FLOAT               NOT NULL
                    COMMENT 'Relative humidity in percent (0.0 – 100.0)',

    pressure        FLOAT               NULL
                    COMMENT 'Barometric pressure in hPa (optional, NULL if sensor does not support it)',

    -- Anomaly flag — updated by DataFilter
    is_anomaly      TINYINT(1)          NOT NULL DEFAULT 0
                    COMMENT '1 if DataFilter flagged this reading as an IQR outlier, 0 otherwise',

    -- Firmware version for traceability
    firmware_ver    VARCHAR(32)         NULL
                    COMMENT 'Firmware version string reported by the ESP32 (e.g. 1.0.0)',

    -- Record insertion time (backend wall clock)
    created_at      TIMESTAMP           NOT NULL DEFAULT CURRENT_TIMESTAMP
                    COMMENT 'Timestamp when the backend inserted this record',

    -- Constraints
    PRIMARY KEY (id),

    -- Composite index: most queries filter by device_id and order/range by reading_time
    INDEX idx_device_time (device_id, reading_time),

    -- Index for anomaly dashboard queries
    INDEX idx_anomaly (device_id, is_anomaly, reading_time)

) ENGINE = InnoDB
  AUTO_INCREMENT = 1
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_unicode_ci
  COMMENT = 'All validated sensor readings received from ESP32-S3 devices via WiFi HTTP POST';


-- ============================================================
-- Table 2: anomaly_events
-- One row per anomaly detection event.
-- A single reading may generate multiple rows (e.g. temp + humidity both anomalous).
-- ============================================================
CREATE TABLE IF NOT EXISTS anomaly_events (
    -- Primary key
    id              BIGINT UNSIGNED     NOT NULL AUTO_INCREMENT,

    -- Link back to sensor_readings (optional — may be NULL if reading was rejected before insert)
    reading_id      BIGINT UNSIGNED     NULL
                    COMMENT 'FK to sensor_readings.id (NULL if reading failed validation)',

    -- Device identification
    device_id       VARCHAR(64)         NOT NULL
                    COMMENT 'Device that produced the anomalous reading',

    -- When the anomaly occurred
    event_time      DATETIME(3)         NOT NULL
                    COMMENT 'Timestamp of the anomalous reading (UTC)',

    -- Which metric was anomalous
    metric          VARCHAR(32)         NOT NULL
                    COMMENT 'Sensor metric name: temperature | humidity | pressure',

    -- The raw observed value
    observed_value  FLOAT               NOT NULL
                    COMMENT 'Actual sensor value that triggered the anomaly',

    -- IQR bounds at the time of detection
    iqr_lower       FLOAT               NOT NULL
                    COMMENT 'Lower bound (Q1 - 1.5*IQR) at detection time',

    iqr_upper       FLOAT               NOT NULL
                    COMMENT 'Upper bound (Q3 + 1.5*IQR) at detection time',

    -- Normalised severity: how many IQR widths outside the fence
    -- severity = |observed - nearest_fence| / IQR
    severity        FLOAT               NOT NULL
                    COMMENT 'Normalised distance outside IQR fence (higher = more extreme)',

    -- Human-readable explanation
    description     VARCHAR(255)        NULL
                    COMMENT 'Optional text description generated by DataFilter',

    -- Record insertion time
    created_at      TIMESTAMP           NOT NULL DEFAULT CURRENT_TIMESTAMP
                    COMMENT 'When the backend inserted this anomaly record',

    -- Constraints
    PRIMARY KEY (id),

    -- FK to sensor_readings (nullable, so no ON DELETE CASCADE — clean up manually if needed)
    INDEX idx_reading (reading_id),

    -- Time-series queries per device
    INDEX idx_device_time (device_id, event_time),

    -- Severity ranking queries
    INDEX idx_severity (device_id, severity DESC)

) ENGINE = InnoDB
  AUTO_INCREMENT = 1
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Anomaly events detected by the IQR sliding-window filter in the C++ DataFilter module';


-- ============================================================
-- Table 3: analysis_log
-- Stores AI-generated analysis from Ollama / Qwen2.5-3B-Instruct.
-- One row per inference call, covering a window of sensor readings.
-- ============================================================
CREATE TABLE IF NOT EXISTS analysis_log (
    -- Primary key
    id              BIGINT UNSIGNED     NOT NULL AUTO_INCREMENT,

    -- Device that was analysed
    device_id       VARCHAR(64)         NOT NULL
                    COMMENT 'Device ID for which the analysis was generated',

    -- Time window covered by this analysis
    window_start    DATETIME(3)         NOT NULL
                    COMMENT 'Earliest reading_time in the sensor window fed to the AI',

    window_end      DATETIME(3)         NOT NULL
                    COMMENT 'Latest reading_time in the sensor window fed to the AI',

    -- How many records were in the window
    window_records  INT UNSIGNED        NOT NULL DEFAULT 0
                    COMMENT 'Number of sensor_readings rows included in the prompt',

    -- How many anomalies were present in the window
    anomaly_count   INT UNSIGNED        NOT NULL DEFAULT 0
                    COMMENT 'Number of anomaly_events in the same time window',

    -- Model metadata
    model_name      VARCHAR(128)        NOT NULL
                    COMMENT 'Ollama model tag used for inference (e.g. qwen2.5:3b-instruct-q4_K_M)',

    -- Token counts (if Ollama reports them)
    prompt_tokens   INT UNSIGNED        NULL
                    COMMENT 'Number of tokens in the prompt (from Ollama response metadata)',

    response_tokens INT UNSIGNED        NULL
                    COMMENT 'Number of tokens in the response (from Ollama response metadata)',

    -- Inference duration
    inference_ms    INT UNSIGNED        NULL
                    COMMENT 'Wall-clock time for the Ollama inference call (milliseconds)',

    -- The AI-generated analysis text
    analysis_text   TEXT                NOT NULL
                    COMMENT 'Full text response from the Qwen2 model',

    -- Record insertion time
    created_at      TIMESTAMP           NOT NULL DEFAULT CURRENT_TIMESTAMP
                    COMMENT 'When the backend inserted this analysis record',

    -- Constraints
    PRIMARY KEY (id),

    -- Time-series queries per device
    INDEX idx_device_time (device_id, window_start),

    -- Most-recent analysis queries
    INDEX idx_created (created_at DESC)

) ENGINE = InnoDB
  AUTO_INCREMENT = 1
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_unicode_ci
  COMMENT = 'AI analysis results produced by AIQueryDispatcher calling Ollama Qwen2.5-3B-Instruct locally';


-- ============================================================
-- Verification queries — run these to confirm setup
-- ============================================================
-- SHOW TABLES;
-- DESCRIBE sensor_readings;
-- DESCRIBE anomaly_events;
-- DESCRIBE analysis_log;
-- SHOW INDEX FROM sensor_readings;
