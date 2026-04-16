# Contributing to Edge-Intelligence IoT Monitor

Thank you for contributing! This document explains how the project is structured, how to set up your development environment, and the conventions to follow.

---

## Table of Contents

1. [Development Environment](#1-development-environment)
2. [Repository Layout](#2-repository-layout)
3. [Backend (C++17)](#3-backend-c17)
4. [Firmware (ESP32-S3)](#4-firmware-esp32-s3)
5. [Database Migrations](#5-database-migrations)
6. [Coding Conventions](#6-coding-conventions)
7. [Commit & Branch Conventions](#7-commit--branch-conventions)
8. [Testing](#8-testing)
9. [Pull Request Checklist](#9-pull-request-checklist)

---

## 1. Development Environment

### Required Tools

| Tool | Version | Purpose |
|------|---------|---------|
| Windows / Linux | — | Dev environment (this repo currently builds on Windows) |
| GCC (MinGW-w64) / Clang | ≥ 12 | C++17 backend compiler |
| CMake | ≥ 3.20 (tested with 4.3.1) | Backend build system |
| SQLite | 3.x | Local database (file-based) |
| Python | ≥ 3.10 | Simulation and test scripts |
| Ollama | latest (optional) | Local LLM runtime (only needed for AI tests) |
| esptool.py | latest | Firmware flashing (Phase 2+) |
| PlatformIO / ESP-IDF | — | Firmware build (Phase 2+) |

### Python Dependencies

```bash
# Needed by scripts/test_ollama.py only:
pip install requests
```

### Install Ollama and Pull Model

```bash
# Install Ollama
curl -fsSL https://ollama.com/install.sh | sh

# Pull the model (downloads ~4.5 GB)
ollama pull qwen2.5:3b-instruct-q4_K_M

# Verify
ollama run qwen2.5:3b-instruct-q4_K_M "Say hello in one sentence."
```

### Database Setup

```bash
# Option A (requires sqlite3 CLI): create a DB file from schema.sql
sqlite3 sensor.db < sql/schema.sql

# Option B: run the ingest tool once; it will execute sql/schema.sql automatically
# and create the DB file if missing.
```

---

## 2. Repository Layout

```
edge-iot-monitor/
├── README.md                  # Bilingual project overview
├── CONTRIBUTING.md            # This file
├── LICENSE
├── .gitignore
├── docs/
│   └── architecture.md        # Full system architecture
├── config/
│   └── config.example.yaml    # Template — copy to config.yaml
├── sql/
│   └── schema.sql             # SQLite schema (3 tables)
├── scripts/
│   ├── simulate_sensor.py     # Generates JSON readings (HTTP POST or --stdout)
│   └── test_ollama.py         # Verifies Ollama/Qwen2 pipeline
├── firmware/
│   └── src/
│       └── main.cpp           # Firmware entry (currently minimal)
└── backend/
    ├── CMakeLists.txt          # Backend build targets
    └── src/                    # C++ backend source (Phase 1, partial)
        ├── StorageEngine.h/.cpp    # SQLite persistence (implemented)
        ├── ingest_stdin.cpp        # Tool: stdin JSON -> SQLite inserts
        ├── DataIngestor.h          # Placeholder (empty)
        ├── DataFilter.h/.cpp       # Placeholder (empty)
        ├── AIQueryDispatcher.h/.cpp # Placeholder (empty)
        └── main.cpp                # Placeholder (empty)
```

---

## 3. Backend (C++17)

### Building

```bash
# Windows (MinGW):
cmake -S . -B build-cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j 8

# If you're using MSVC, pick a Visual Studio generator instead.

# Build artifact:
# - Windows: build-cmake/backend/edge_ingest.exe
```

### Module Responsibilities

| Module | File(s) | Role |
|--------|---------|------|
| `DataIngestor` | DataIngestor.h/.cpp | HTTP server, JSON parse & validate (TODO) |
| `DataFilter` | DataFilter.h/.cpp | Sliding IQR anomaly detection (TODO) |
| `StorageEngine` | StorageEngine.h/.cpp | SQLite persistence |
| `AIQueryDispatcher` | AIQueryDispatcher.h/.cpp | Prompt build, Ollama call, log result (TODO) |

### Adding a New C++ Module

1. Create `backend/src/MyModule.h` and `backend/src/MyModule.cpp`
2. Add to `backend/CMakeLists.txt`
3. Wire into `main.cpp`
4. Write a unit test in `backend/tests/`

---

## 4. Firmware (ESP32-S3)

> Phase 2 and beyond. No STM32, no UART-to-backend communication. The ESP32-S3 connects to the backend **exclusively via WiFi HTTP POST**.

### Toolchain Options

**Option A — ESP-IDF v5.x (recommended for full control)**
```bash
# Follow https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Option B — Arduino Core for ESP32**
```bash
# Use Arduino IDE 2.x or arduino-cli
# Board: "ESP32S3 Dev Module"
# Flash size: 16MB, PSRAM: OPI PSRAM (8MB)
```

### Flashing with esptool.py

```bash
pip install esptool

# Full flash (combined binary from ESP-IDF)
esptool.py --chip esp32s3 \
           --port /dev/ttyUSB0 \
           --baud 921600 \
           write_flash 0x0 build/firmware.bin

# Verify after flash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 verify_flash 0x0 build/firmware.bin
```

### I²C Pin Assignment

```
GPIO 8  →  SDA  (sensors + OLED)
GPIO 9  →  SCL  (sensors + OLED)
```

Do **not** change these without updating `config.yaml` and the firmware I²C init.

### Firmware Conventions

- All sensor reads go through `MedianFilter<float, 5>` before transmission
- WiFi credentials are stored in `config.yaml` (never hardcoded)
- HTTP POST target: `http://<backend_ip>:8080/api/ingest`
- JSON payload must include: `device_id`, `timestamp`, `temperature`, `humidity`
- Retry HTTP POST up to 3 times on failure before sleeping

---

## 5. Database Migrations

- All schema changes go in `sql/schema.sql`
- For incremental migrations, add a new file: `sql/migration_v2.sql`, etc.
- Never modify existing column names without updating all backend prepared statements
- After any schema change, re-apply schema (or create a fresh DB): `sqlite3 sensor.db < sql/schema.sql`

---

## 6. Coding Conventions

### C++ (Backend & Firmware)

- Standard: **C++17**
- Naming:
  - Classes: `PascalCase`
  - Functions/variables: `camelCase`
  - Constants/macros: `UPPER_SNAKE_CASE`
- All public functions must have a Doxygen comment (`/** ... */`)
- No raw owning pointers — use `std::unique_ptr` / `std::shared_ptr`
- No `using namespace std;` in header files
- Format with `clang-format` (style: Google)

### Python (Scripts)

- Standard: **Python 3.10+**
- Style: PEP 8 (use `black` formatter)
- All scripts must have a `main()` function and `if __name__ == "__main__"` guard
- Use `requests` for HTTP, `PyYAML` for config

### SQL

- Table and column names: `snake_case`
- All time-series tables should have: composite index on `(device_id, timestamp)`

---

## 7. Commit & Branch Conventions

### Branch Names

```
feature/<short-description>     # New feature
fix/<short-description>         # Bug fix
docs/<short-description>        # Documentation only
refactor/<short-description>    # Code cleanup, no functional change
```

### Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
feat(ingestor): add JSON schema validation for humidity range
fix(filter): correct IQR window eviction logic for fast sampling
docs(architecture): update data flow diagram for Phase 2
refactor(storage): replace raw pointer with unique_ptr in pool
```

---

## 8. Testing

### Backend Unit Tests

```bash
# (Unit tests not wired yet in this repo snapshot.)
# When tests are added, they should be runnable via CTest from the CMake build dir.
```

### Integration Tests (simulate full pipeline)

```bash
# Terminal 3: check SQLite (if you have sqlite3 CLI)
sqlite3 sensor.db "SELECT COUNT(*) FROM sensor_readings;"

# Quick local ingest without HTTP:
python scripts/simulate_sensor.py --count 100 --stdout --interval 0.5 | build-cmake/backend/edge_ingest.exe --db sensor.db
```

### Ollama Test

```bash
python3 scripts/test_ollama.py
```

---

## 9. Pull Request Checklist

Before opening a PR, confirm all of the following:

- [ ] Code compiles without warnings (`-Wall -Wextra`)
- [ ] No `STM32`, `UART`, `SWD`, `CubeMX`, `HAL_`, or `Serial.print` references anywhere
- [ ] All new C++ functions have Doxygen comments
- [ ] `clang-format` applied to all changed `.cpp`/`.h` files
- [ ] `black` applied to all changed `.py` files
- [ ] Any new config keys added to `config/config.example.yaml`
- [ ] Any schema changes reflected in `sql/schema.sql`
- [ ] `simulate_sensor.py` still passes end-to-end
- [ ] PR description explains **what** changed and **why**
