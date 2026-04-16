# Edge-Intelligence IoT Monitoring & Analysis System
# 边缘智能物联网监测与分析系统

<div align="center">

![Platform](https://img.shields.io/badge/MCU-ESP32--S3--N16R8-blue)
![Backend](https://img.shields.io/badge/Backend-C%2B%2B17-orange)
![AI](https://img.shields.io/badge/AI-Ollama%20%2B%20Qwen2.5--3B-green)
![DB](https://img.shields.io/badge/Database-SQLite%203-lightblue)
![License](https://img.shields.io/badge/License-MIT-yellow)

</div>

---

## English

### Overview

An end-to-end edge-intelligence IoT system that collects environmental sensor data on an **ESP32-S3-N16R8** microcontroller, transmits it over **WiFi via HTTP POST** to a **C++17 backend**, performs real-time **IQR-based anomaly detection**, stores results in **SQLite 3**, and runs **fully local AI inference** (Ollama + Qwen2.5-3B-Instruct) for contextual analysis — with no cloud dependency.

### Key Features

- **Local AI inference** — Qwen2.5-3B-Instruct (Q4_K_M) via Ollama; zero cloud calls
- **Real-time anomaly detection** — sliding IQR filter (60 s window, 1.5× multiplier)
- **Wireless data pipeline** — ESP32 WiFi → HTTP POST → C++ backend
- **On-device signal conditioning** — median filter template on ESP32-S3
- **OLED feedback loop** — AI analysis results pushed back to 128×64 SSD1306 display
- **Modular C++ backend** — DataIngestor · DataFilter · StorageEngine · AIQueryDispatcher

### Hardware

| Component | Part | Interface |
|-----------|------|-----------|
| MCU | ESP32-S3-N16R8 (16 MB Flash, 8 MB PSRAM) | — |
| Display | OLED 128×64 SSD1306 | I²C |
| Temp/Humidity | SHT31 or BME280 | I²C |
| Network | Built-in 2.4 GHz WiFi (802.11 b/g/n) | — |

### Software Stack

| Layer | Technology |
|-------|-----------|
| Firmware | ESP-IDF v5.x / Arduino Core for ESP32 |
| Flashing | esptool.py |
| Backend | C++17, CMake, Ubuntu 22.04 |
| Database | SQLite 3 |
| AI Runtime | Ollama + Qwen2.5-3B-Instruct Q4_K_M |
| Communication | WiFi → HTTP POST `/api/ingest` |

### Repository Structure

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
│   └── schema.sql
├── scripts/
│   ├── simulate_sensor.py
│   └── test_ollama.py
├── firmware/
│   └── src/               # ESP32-S3 firmware
└── backend/
    └── src/               # C++ backend modules
```

### Quick Start

#### Phase 1 — Backend + Simulated Data (no hardware required)

```bash
# 1. Clone the repo
git clone https://github.com/<you>/edge-iot-monitor.git
cd edge-iot-monitor

# 2. Create the database (optional)
# Option A: create SQLite DB via sqlite3 CLI (if installed)
sqlite3 sensor.db < sql/schema.sql
# Option B: let the backend create tables automatically on first run

# 3. Copy and edit config
cp config/config.example.yaml config/config.yaml
# Edit DB credentials and Ollama host

# 4. Build the C++ backend
cd backend
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/edge_backend

# 5. Simulate sensor data
python3 scripts/simulate_sensor.py

# 6. Test AI pipeline
python3 scripts/test_ollama.py
```

#### Phase 2 — Flash ESP32-S3 Firmware

```bash
# Install esptool
pip install esptool

# Flash (adjust port as needed)
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 firmware.bin
```

### Data Flow

```
ESP32-S3 (sensor read)
  └─ MedianFilter (on-device, C++ template)
       └─ HTTP POST /api/ingest  (WiFi, JSON)
            └─ DataIngestor (C++ backend)
                 ├─ DataFilter → anomaly_events (SQLite)
                 ├─ StorageEngine → sensor_readings (SQLite)
                 └─ AIQueryDispatcher
                      └─ Ollama :11434 (Qwen2.5-3B)
                           └─ analysis_log (SQLite)
                                └─ HTTP response → OLED display
```

### Development Phases

| Phase | Goal | Status |
|-------|------|--------|
| 1 | Backend + SQLite + simulated data + Ollama | 🔧 In Progress |
| 2 | ESP32-S3 firmware + real sensors | ⏳ Planned |
| 3 | OLED feedback loop (AI → display) | ⏳ Planned |

---

## 中文

### 项目概述

本项目构建了一个完整的**边缘智能物联网监测系统**。  
**ESP32-S3-N16R8** 微控制器采集环境传感器数据，通过 **WiFi / HTTP POST** 将数据上传至 **C++17 后端服务**，后端执行基于 **IQR 的实时异常检测**，将结果存入 **SQLite 3**，并调用本地部署的 **Ollama + Qwen2.5-3B-Instruct** 进行上下文分析 —— 全程零云端依赖。

### 核心特性

- **本地 AI 推理** — Qwen2.5-3B-Instruct（Q4_K_M 量化）通过 Ollama 运行，无任何云端调用
- **实时异常检测** — 滑动 IQR 滤波（60 秒窗口，1.5 倍乘数）
- **无线数据链路** — ESP32 WiFi → HTTP POST → C++ 后端
- **设备端信号调理** — ESP32-S3 上的中值滤波器（C++ 模板实现）
- **OLED 反馈回路** — AI 分析结果推送至 128×64 SSD1306 显示屏
- **模块化 C++ 后端** — DataIngestor · DataFilter · StorageEngine · AIQueryDispatcher

### 硬件清单

| 组件 | 型号 | 接口 |
|------|------|------|
| 微控制器 | ESP32-S3-N16R8（16 MB Flash，8 MB PSRAM） | — |
| 显示屏 | OLED 128×64 SSD1306 | I²C |
| 温湿度传感器 | SHT31 或 BME280 | I²C |
| 无线网络 | 内置 2.4 GHz WiFi（802.11 b/g/n） | — |

### 软件栈

| 层级 | 技术选型 |
|------|---------|
| 固件 | ESP-IDF v5.x / Arduino Core for ESP32 |
| 烧录工具 | esptool.py |
| 后端 | C++17，CMake，Ubuntu 22.04 |
| 数据库 | SQLite 3 |
| AI 运行时 | Ollama + Qwen2.5-3B-Instruct Q4_K_M |
| 通信协议 | WiFi → HTTP POST `/api/ingest` |

### 快速启动

#### 阶段一 —— 后端 + 模拟数据（无需硬件）

```bash
# 1. 克隆仓库
git clone https://github.com/<你的用户名>/edge-iot-monitor.git
cd edge-iot-monitor

# 2. 初始化数据库（可选）
# 方式 A：使用 sqlite3 CLI 初始化（如果你装了 sqlite3）
sqlite3 sensor.db < sql/schema.sql
# 方式 B：首次运行后端时自动建表

# 3. 复制并编辑配置文件
cp config/config.example.yaml config/config.yaml
# 修改数据库凭据和 Ollama 地址

# 4. 编译 C++ 后端
cd backend
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/edge_backend

# 5. 运行传感器模拟器
python3 scripts/simulate_sensor.py

# 6. 测试 AI 推理管道
python3 scripts/test_ollama.py
```

#### 阶段二 —— 烧录 ESP32-S3 固件

```bash
# 安装 esptool
pip install esptool

# 烧录固件（根据实际端口修改）
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 firmware.bin
```

### 开发阶段规划

| 阶段 | 目标 | 状态 |
|------|------|------|
| 阶段一 | 后端 + SQLite + 模拟数据 + Ollama | 🔧 进行中 |
| 阶段二 | ESP32-S3 固件 + 真实传感器 | ⏳ 计划中 |
| 阶段三 | OLED 反馈回路（AI 结果 → 显示屏） | ⏳ 计划中 |

---

## License / 许可证

MIT © 2025
