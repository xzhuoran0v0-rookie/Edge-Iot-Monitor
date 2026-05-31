# Edge-Intelligence IoT Monitoring & Analysis System

A competition IoT project featuring local AI-powered sensor analysis, anomaly detection, and real-time display feedback — with zero cloud dependency.

---

## Hardware

| Component | Model | Notes |
|---|---|---|
| MCU | ESP32-S3-N16R8 | 16MB Flash, 8MB PSRAM, built-in WiFi |
| Display | 0.96" OLED 128×64 | SSD1315 driver, I²C |
| Sensor | SHT30 | Temperature & humidity, I²C |
| Communication | ESP32-S3 built-in WiFi | HTTP POST to backend |
| Power | USB | — |
| Host Machine | macOS, Apple M5 | Runs Ollama + C++ backend |

---

## Software Stack

| Layer | Technology |
|---|---|
| Firmware | ESP-IDF v5.x or Arduino Core for ESP32 |
| Backend | C++17, CMake, running on macOS |
| Database | SQLite |
| AI | Ollama + Qwen2.5-3B |
| Communication | ESP32 WiFi → HTTP POST to macOS backend |

---

## Repository Structure

```
edge-iot-monitor/
├── README.md
├── LICENSE
├── .gitignore
├── CONTRIBUTING.md
├── docs/
│   └── architecture.md
├── config/
│   └── config.example.yaml
├── sql/
│   └── schema.sql              ← 3 tables: sensor_readings, anomaly_events, analysis_log
├── scripts/
│   ├── simulate_sensor.py      ← simulates sensor POST without hardware
│   └── test_ollama.py          ← verifies Ollama/Qwen2.5 is working
├── firmware/
│   └── src/                    ← ESP32-S3 code (Phase 2)
└── backend/
    └── src/                    ← C++ modules
        ├── DataIngestor         ← HTTP server, receives & validates JSON
        ├── DataFilter           ← sliding IQR anomaly detection
        ├── StorageEngine        ← SQLite connection, batch insert, query helpers
        └── AIQueryDispatcher    ← builds prompts, calls Ollama, parses response
```

---

## Data Flow

```
ESP32-S3 (SHT30 sensor)
  → MedianFilter (on-device, template C++)
  → HTTP POST /api/ingest (JSON payload)
  → macOS C++ backend
      → DataFilter (IQR anomaly detection, 60s window, 1.5× multiplier)
      → SQLite (sensor_readings / anomaly_events tables)
      → AIQueryDispatcher (every N records)
          → Ollama localhost:11434
          → Qwen2.5-3B inference (Apple M5)
          → analysis_log table
  → (optional) result pushed back to ESP32 → OLED display
```

---

## C++ Backend Modules

- **DataIngestor** — HTTP server, receives JSON from ESP32, validates input
- **DataFilter** — sliding IQR anomaly detection (60s window, 1.5× multiplier)
- **StorageEngine** — SQLite connection pool, batch insert, query helpers
- **AIQueryDispatcher** — builds prompts from sensor windows, calls Ollama REST API, parses response

---

## Key Design Decisions

- All AI inference is **local** via Ollama — zero cloud dependency
- ESP32-S3 chosen over STM32 for built-in WiFi
- Qwen2.5-3B (~1.9GB, >95% accuracy retention vs full precision)
- SQLite for lightweight, zero-config local storage
- SSD1315 driver used for OLED (note: not SSD1306 compatible)

---

## Development Phases

### Phase 1 — Backend + AI Pipeline (current)
- Set up SQLite schema
- Implement C++ backend (DataIngestor, StorageEngine, DataFilter, AIQueryDispatcher)
- Verify with `scripts/simulate_sensor.py` (no hardware needed)
- Verify AI pipeline with `scripts/test_ollama.py`

### Phase 2 — Firmware
- Write ESP32-S3 firmware
- Connect SHT30 sensor
- Replace simulator with real sensor data

### Phase 3 — Close the Loop
- Push LLM analysis results back to ESP32
- Display results on OLED

---

## Requirements

### Host Machine (macOS / Apple M5)
- [Ollama](https://ollama.com) with `qwen2.5:3b` model
- CMake 4.x
- Apple Clang (Xcode Command Line Tools)
- VS Code with C/C++, CMake, CMake Tools extensions

### ESP32-S3 (Phase 2)
- ESP-IDF v5.x or Arduino Core for ESP32
- SHT30 library
- SSD1315-compatible OLED library

---

## Getting Started

### 1. Pull Qwen2.5 model
```bash
ollama pull qwen2.5:3b
```

### 2. Build the backend
```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### 3. Run the simulator
```bash
python scripts/simulate_sensor.py
```

### 4. Test AI pipeline
```bash
python scripts/test_ollama.py
```

---

## License

MIT
