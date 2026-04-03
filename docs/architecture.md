# System Architecture
# 系统架构文档

**Project**: Edge-Intelligence IoT Monitoring & Analysis System  
**Version**: 1.0  
**Last Updated**: 2025

---

## 1. High-Level Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        EDGE DEVICE                              │
│                                                                 │
│  ┌──────────┐    I²C    ┌─────────────┐                        │
│  │ SHT31 /  │──────────▶│  ESP32-S3   │                        │
│  │ BME280   │           │  N16R8      │                        │
│  └──────────┘           │             │                        │
│                         │ MedianFilter│                        │
│  ┌──────────┐    I²C    │ (C++template│                        │
│  │ SSD1306  │◀──────────│  on-device) │                        │
│  │ OLED     │           │             │                        │
│  │ 128×64   │           └──────┬──────┘                        │
│  └──────────┘                  │ WiFi 802.11 b/g/n             │
└───────────────────────────────┼─────────────────────────────────┘
                                 │
                    HTTP POST /api/ingest
                    (JSON payload, port 8080)
                                 │
┌───────────────────────────────▼─────────────────────────────────┐
│                     LINUX BACKEND (Ubuntu 22.04 / VMware VM)    │
│                                                                 │
│  ┌─────────────────┐                                           │
│  │  DataIngestor   │  HTTP server · JSON validation            │
│  │  (C++17)        │  Input sanitisation · Schema check        │
│  └────────┬────────┘                                           │
│           │                                                     │
│  ┌────────▼────────┐                                           │
│  │  DataFilter     │  Sliding IQR anomaly detection            │
│  │  (C++17)        │  60 s window · 1.5× IQR multiplier        │
│  └────────┬────────┘                                           │
│           │                                                     │
│  ┌────────▼────────┐                                           │
│  │  StorageEngine  │  MySQL connection pool · Batch insert     │
│  │  (C++17)        │  Composite index (device_id, timestamp)   │
│  └────────┬────────┘                                           │
│           │                                                     │
│  ┌────────▼──────────┐                                         │
│  │ AIQueryDispatcher │  Builds prompts from sensor windows     │
│  │ (C++17)           │  REST call → Ollama :11434              │
│  └────────┬──────────┘  Parses response → analysis_log        │
│           │                                                     │
│  ┌────────▼────────────────────────────────┐                  │
│  │           MySQL 8.0 (InnoDB)            │                  │
│  │  sensor_readings · anomaly_events ·     │                  │
│  │  analysis_log                           │                  │
│  └─────────────────────────────────────────┘                  │
│                                                                 │
│  ┌──────────────────────────────────────────┐                  │
│  │  Ollama Runtime (localhost:11434)         │                  │
│  │  Model: Qwen2.5-3B-Instruct Q4_K_M        │                  │
│  │  RAM usage: ~1.9 GB · 100% local           │                  │
│  └──────────────────────────────────────────┘                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Hardware Layer

### 2.1 ESP32-S3-N16R8

| Attribute | Value |
|-----------|-------|
| CPU | Xtensa LX7 dual-core, up to 240 MHz |
| Flash | 16 MB (Quad SPI) |
| PSRAM | 8 MB (Octal SPI) |
| WiFi | 802.11 b/g/n, 2.4 GHz, built-in |
| Firmware framework | ESP-IDF v5.x or Arduino Core for ESP32 |
| Flashing tool | esptool.py |
| Operating voltage | 3.3 V |

The ESP32-S3 was chosen over alternatives due to its built-in WiFi (eliminating any external network module), large PSRAM for buffering sensor windows, and strong community support for both ESP-IDF and Arduino ecosystems.

### 2.2 Sensors

**Primary option — SHT31:**
- Temperature: ±0.2 °C accuracy, –40 to +125 °C range
- Humidity: ±2% RH accuracy
- Interface: I²C (default address 0x44)

**Alternative — BME280:**
- Temperature, Humidity, and Barometric Pressure
- I²C (address 0x76 or 0x77)

### 2.3 Display — SSD1306 OLED 128×64

- Interface: I²C (address 0x3C)
- Used to display: current readings, WiFi status, anomaly alerts, AI analysis summary
- Driven by the U8g2 library (ESP-IDF) or Adafruit SSD1306 (Arduino)

### 2.4 I²C Bus Wiring

```
ESP32-S3          SHT31 / BME280      SSD1306 OLED
GPIO 8 (SDA) ─────── SDA ─────────── SDA
GPIO 9 (SCL) ─────── SCL ─────────── SCL
3.3 V ────────────── VIN ─────────── VCC
GND ──────────────── GND ─────────── GND
```

> Both sensors and display share the same I²C bus. Each device has a unique 7-bit address.

---

## 3. Firmware Layer (ESP32-S3)

### 3.1 On-Device MedianFilter

```cpp
// Template class — works for any numeric type and window size
template <typename T, size_t N>
class MedianFilter {
    T buffer[N];
    size_t index = 0;
    bool full = false;
public:
    void push(T value);
    T compute() const;  // returns median without modifying buffer
};
```

**Why median filtering?**  
Median filters are better than moving averages for sensor data because they reject single-sample spikes (e.g., I²C glitches, EMI-induced readings) while preserving real step changes.

### 3.2 WiFi Communication

- Protocol: HTTP/1.1 POST to `http://<backend_ip>:8080/api/ingest`
- Payload format: `application/json`
- Retry logic: 3 attempts with exponential back-off on failure
- On success: parse HTTP 200 response, extract AI analysis text, display on OLED

### 3.3 JSON Payload Format

```json
{
  "device_id": "esp32s3-001",
  "timestamp": 1700000000,
  "temperature": 24.3,
  "humidity": 58.7,
  "pressure": 1013.2,
  "firmware_version": "1.0.0"
}
```

---

## 4. Backend Layer (C++17, Ubuntu 22.04)

### 4.1 DataIngestor

**Responsibility:** HTTP server that accepts POST requests from ESP32 devices.

- Listens on `0.0.0.0:8080`
- Endpoint: `POST /api/ingest`
- Validates JSON schema (required fields, type checks, range checks)
- Rejects malformed or out-of-range payloads with HTTP 400
- Passes valid `SensorReading` structs downstream

**Input validation rules:**

| Field | Type | Range |
|-------|------|-------|
| device_id | string | 1–64 chars |
| timestamp | uint64 | Unix epoch, > 0 |
| temperature | float | –40.0 to +85.0 °C |
| humidity | float | 0.0 to 100.0 % RH |
| pressure | float | 800.0 to 1200.0 hPa (optional) |

### 4.2 DataFilter

**Responsibility:** Real-time IQR-based anomaly detection.

**Algorithm:**
```
For each incoming reading:
  1. Append value to sliding window (last 60 seconds of readings)
  2. Sort window values
  3. Compute Q1 (25th percentile) and Q3 (75th percentile)
  4. IQR = Q3 - Q1
  5. Lower bound = Q1 - 1.5 × IQR
  6. Upper bound = Q3 + 1.5 × IQR
  7. If reading < lower OR reading > upper → flag as anomaly
  8. Write anomaly record to anomaly_events table
```

**Window management:**
- Window size: 60 seconds of data (dynamic, based on timestamps)
- Old entries evicted as time advances
- Separate windows per `device_id`

### 4.3 StorageEngine

**Responsibility:** MySQL connection pooling and persistence.

- Connection pool: 4–8 connections (configurable)
- Batch insert: accumulates up to 50 readings before flushing
- Flush also triggered on 5-second timeout
- Prepared statements for injection safety
- Handles reconnection on MySQL timeout

**Tables managed:**
- `sensor_readings` — every validated reading
- `anomaly_events` — flagged anomalies with severity score
- `analysis_log` — AI-generated analysis text

### 4.4 AIQueryDispatcher

**Responsibility:** Periodic LLM analysis of sensor data windows.

- Trigger: every N new records (configurable, default 20)
- Queries last 100 readings from MySQL
- Builds a structured prompt (see below)
- POSTs to `http://localhost:11434/api/generate` (Ollama REST API)
- Streams response, accumulates full text
- Writes result to `analysis_log`
- Optionally returns summary in HTTP response to ESP32

**Prompt template:**
```
You are an environmental monitoring AI assistant.
Analyse the following sensor data window and provide:
1. A trend summary (2-3 sentences)
2. Any anomalies or concerns
3. A recommended action if warranted

Data window (last 100 readings from device {device_id}):
Timestamps: {timestamps}
Temperatures (°C): {temps}
Humidity (%RH): {hums}

Detected anomalies: {anomaly_count}
Respond concisely. Maximum 150 words.
```

---

## 5. Database Layer (MySQL 8.0)

### 5.1 Tables

**`sensor_readings`**
```sql
CREATE TABLE sensor_readings (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    device_id     VARCHAR(64)    NOT NULL,
    timestamp     DATETIME(3)    NOT NULL,
    temperature   FLOAT          NOT NULL,
    humidity      FLOAT          NOT NULL,
    pressure      FLOAT          NULL,
    is_anomaly    TINYINT(1)     NOT NULL DEFAULT 0,
    created_at    TIMESTAMP      DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_device_time (device_id, timestamp)
) ENGINE=InnoDB;
```

**`anomaly_events`**
```sql
CREATE TABLE anomaly_events (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    device_id     VARCHAR(64)    NOT NULL,
    timestamp     DATETIME(3)    NOT NULL,
    metric        VARCHAR(32)    NOT NULL,
    observed      FLOAT          NOT NULL,
    iqr_lower     FLOAT          NOT NULL,
    iqr_upper     FLOAT          NOT NULL,
    severity      FLOAT          NOT NULL,
    created_at    TIMESTAMP      DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_device_time (device_id, timestamp)
) ENGINE=InnoDB;
```

**`analysis_log`**
```sql
CREATE TABLE analysis_log (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    device_id     VARCHAR(64)    NOT NULL,
    window_start  DATETIME(3)    NOT NULL,
    window_end    DATETIME(3)    NOT NULL,
    model_name    VARCHAR(128)   NOT NULL,
    prompt_tokens INT            NULL,
    analysis_text TEXT           NOT NULL,
    created_at    TIMESTAMP      DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_device_time (device_id, window_start)
) ENGINE=InnoDB;
```

### 5.2 Indexing Strategy

The composite index `(device_id, timestamp)` on all three tables ensures efficient:
- Time-range queries for a specific device (most common query pattern)
- Window extraction for the DataFilter and AIQueryDispatcher
- `ORDER BY timestamp` without filesort

---

## 6. AI Layer (Ollama + Qwen2.5-3B-Instruct)

| Attribute | Value |
|-----------|-------|
| Model | Qwen2.5-3B-Instruct |
| Quantization | Q4_K_M |
| VRAM / RAM | ~1.9 GB |
| Accuracy retention | >95% vs full precision |
| Inference endpoint | `http://localhost:11434/api/generate` |
| Cloud dependency | None — fully local |

**Why Qwen2.5-3B Q4_K_M?**
- 3B parameter class is lightweight enough to run comfortably on CPU with low RAM usage (~1.9 GB)
- Q4_K_M quantization gives the best size/quality tradeoff in the 4-bit family
- Qwen2.5 improves over Qwen2 in instruction following, structured output, and multilingual support
- Response latency ~5–15 s on CPU, well suited for periodic analysis

---

## 7. Communication Protocol

### 7.1 ESP32 → Backend (Data Ingestion)

```
Method:   POST
Endpoint: http://<backend_ip>:8080/api/ingest
Headers:  Content-Type: application/json
Body:     { "device_id": "...", "timestamp": ..., "temperature": ..., "humidity": ... }

Success response (200):
{ "status": "ok", "anomaly": false }

Anomaly response (200):
{ "status": "ok", "anomaly": true, "analysis": "<AI text if available>" }

Error response (400):
{ "status": "error", "message": "Invalid temperature range" }
```

### 7.2 Backend → Ollama (AI Inference)

```
Method:   POST
Endpoint: http://localhost:11434/api/generate
Body:     { "model": "qwen2.5:3b-instruct-q4_K_M", "prompt": "...", "stream": false }
Response: { "response": "<analysis text>", "done": true }
```

---

## 8. Development Phases

### Phase 1 — Backend + Simulated Data
- Implement DataIngestor, DataFilter, StorageEngine, AIQueryDispatcher
- Set up MySQL schema
- Validate with `simulate_sensor.py` and `test_ollama.py`
- No hardware required

### Phase 2 — ESP32-S3 Firmware
- Implement sensor reading loop (SHT31 or BME280)
- Implement MedianFilter on-device
- Implement WiFi connection + HTTP POST
- Replace simulator with real hardware

### Phase 3 — Closed Loop (OLED Feedback)
- Backend includes AI analysis in HTTP response
- ESP32 parses response
- Display summary on SSD1306 OLED
- Alert animations for anomaly events

---

## 9. Security Considerations

- The HTTP endpoint is intended for a **local network only** (lab / competition environment)
- `device_id` is validated against an allowlist (configurable)
- All MySQL queries use **prepared statements** (no string concatenation)
- Ollama is bound to `localhost` only — not exposed externally
- Configuration secrets (DB password) are loaded from `config.yaml`, not hardcoded

---

## 10. Glossary

| Term | Definition |
|------|-----------|
| IQR | Interquartile Range — a robust statistical measure of spread (Q3 − Q1) |
| Q4_K_M | A specific quantization scheme: 4-bit weights, K-quants, medium block size |
| Ollama | Open-source local LLM runtime that serves models via REST API |
| ESP-IDF | Espressif IoT Development Framework — the official RTOS-based SDK for ESP32 |
| esptool.py | Python utility for flashing firmware to ESP32 chips over USB |
| MedianFilter | Signal processing filter that outputs the median of the N most recent samples |
