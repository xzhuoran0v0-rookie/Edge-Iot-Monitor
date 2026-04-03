-- ============================================================
-- Edge-IoT Monitor — Database Schema
-- Run: mysql -u root -p < sql/schema.sql
-- ============================================================

CREATE DATABASE IF NOT EXISTS sensor_db
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE sensor_db;

-- ------------------------------------------------------------
-- Table 1: sensor_readings
-- Stores every validated sensor sample from all devices.
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS sensor_readings (
    id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    device_id   VARCHAR(64)      NOT NULL,
    timestamp   DATETIME(3)      NOT NULL,          -- millisecond precision
    temperature DECIMAL(6,2)     DEFAULT NULL,      -- °C
    humidity    DECIMAL(6,2)     DEFAULT NULL,      -- %RH
    pressure    DECIMAL(8,2)     DEFAULT NULL,      -- hPa
    extra_json  JSON             DEFAULT NULL,      -- extensible: any other sensors
    created_at  TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (id),
    INDEX idx_device_time (device_id, timestamp),  -- primary query pattern
    INDEX idx_timestamp   (timestamp)              -- global time-range scans
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ------------------------------------------------------------
-- Table 2: anomaly_events
-- Records flagged outliers detected by the C++ IQR filter.
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS anomaly_events (
    id              BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    device_id       VARCHAR(64)      NOT NULL,
    detected_at     DATETIME(3)      NOT NULL,
    metric          VARCHAR(32)      NOT NULL,  -- e.g. 'temperature', 'humidity'
    observed_value  DECIMAL(10,4)    NOT NULL,
    expected_range  VARCHAR(64)      DEFAULT NULL,  -- e.g. '18.00 ~ 32.00'
    severity        ENUM('LOW','MEDIUM','HIGH') NOT NULL DEFAULT 'MEDIUM',
    created_at      TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (id),
    INDEX idx_device_detected (device_id, detected_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ------------------------------------------------------------
-- Table 3: analysis_log
-- Stores LLM inference results from Ollama/Qwen2.
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS analysis_log (
    id              BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT,
    device_id       VARCHAR(64)      NOT NULL,
    analyzed_at     DATETIME(3)      NOT NULL,
    window_start    DATETIME(3)      NOT NULL,  -- data window fed to LLM
    window_end      DATETIME(3)      NOT NULL,
    severity        ENUM('NORMAL','LOW','MEDIUM','HIGH','CRITICAL') NOT NULL,
    diagnosis       TEXT             NOT NULL,
    recommendation  TEXT             DEFAULT NULL,
    raw_response    TEXT             DEFAULT NULL,  -- full LLM output for audit
    created_at      TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (id),
    INDEX idx_device_analyzed (device_id, analyzed_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- ------------------------------------------------------------
-- Create a dedicated application user (principle of least privilege)
-- Replace 'YOUR_PASSWORD' before running.
-- ------------------------------------------------------------
CREATE USER IF NOT EXISTS 'iot_user'@'localhost' IDENTIFIED BY 'YOUR_PASSWORD';
GRANT SELECT, INSERT, UPDATE ON sensor_db.* TO 'iot_user'@'localhost';
FLUSH PRIVILEGES;

-- Verify
SHOW TABLES;
