# Edge-Intelligence IoT Monitoring & Analysis System
# 基于边缘智能的物联网数据监测与决策系统

<p align="center">
  <img src="https://img.shields.io/badge/Platform-STM32-blue?style=for-the-badge&logo=stmicroelectronics" />
  <img src="https://img.shields.io/badge/Backend-C%2B%2B17-00599C?style=for-the-badge&logo=cplusplus" />
  <img src="https://img.shields.io/badge/Database-MySQL-4479A1?style=for-the-badge&logo=mysql&logoColor=white" />
  <img src="https://img.shields.io/badge/LLM-Qwen2%20%7C%20Ollama-FF6B35?style=for-the-badge" />
  <img src="https://img.shields.io/badge/OS-Ubuntu%2022.04-E95420?style=for-the-badge&logo=ubuntu&logoColor=white" />
  <img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" />
</p>

> **A full-stack, privacy-first IoT prototype that fuses embedded hardware with on-premise AI inference — no cloud dependency, no data leakage.**
>
> **一个完全私有化部署的物联网原型系统，将嵌入式硬件与本地 AI 推理深度融合——零云端依赖，零数据泄露。**

---

## 📋 Table of Contents | 目录

- [Project Overview](#-project-overview--项目概述)
- [Key Features](#-key-features--核心特性)
- [System Architecture](#-system-architecture--系统架构)
- [Algorithm & Logic](#-algorithm--logic--算法与核心逻辑)
- [Tech Stack](#-tech-stack--技术栈)
- [Installation & Setup](#-installation--setup--环境配置)
- [Future Roadmap](#-future-roadmap--未来规划)
- [License](#-license)

---

## 🌐 Project Overview | 项目概述

### English

This project is an **edge-intelligent IoT monitoring system** designed for industrial and environmental sensing scenarios. Unlike conventional cloud-dependent IoT pipelines, all data processing, storage, and AI inference happen **entirely on local infrastructure** — from the STM32 microcontroller at the sensing layer to the Ubuntu Linux server running a quantized large language model.

The core innovation lies in the convergence of two paradigms:

1. **Edge Computing** — A C++ backend on a local Linux server handles real-time data ingestion, filtering, and persistence with sub-millisecond latency, eliminating round-trip overhead to remote cloud nodes.
2. **Local AI Inference** — A quantized Qwen2 model, served via Ollama, performs intelligent analysis on the aggregated sensor data, providing contextual diagnostics and actionable recommendations without any data leaving the private network perimeter.

The result is a closed-loop system: sensors capture → embedded firmware processes → Linux backend aggregates → LLM reasons → OLED reflects — all within a self-contained, auditable environment suitable for sensitive industrial deployments.

### 中文

本项目是一套面向工业与环境感知场景的**边缘智能物联网监测系统**。与传统依赖云端的 IoT 链路不同，所有的数据处理、存储与 AI 推理均**完全在本地基础设施上完成**——从感知层的 STM32 微控制器，到运行量化大语言模型的 Ubuntu Linux 服务器，全程私有化。

核心创新体现在两种范式的交汇：

1. **边缘计算** —— 本地 Linux 服务器上的 C++ 后端以亚毫秒级延迟完成实时数据接入、过滤与持久化，彻底消除云端往返开销。
2. **本地 AI 推理** —— 通过 Ollama 部署的量化 Qwen2 模型，对聚合后的传感器数据执行智能分析，提供上下文感知的诊断与可操作建议，全程数据不出私有网络边界。

最终形成一套闭环系统：传感器采集 → 嵌入式固件处理 → Linux 后端聚合 → LLM 推理 → OLED 反馈，整套链路运行在完全自包含、可审计的私有环境中，适用于对数据安全性要求严苛的工业场景。

---

## ✨ Key Features | 核心特性

### 🔌 Real-Time Hardware Data Acquisition & OLED Dynamic Display
**实时硬件数据采集与 OLED 动态显示**

- STM32 firmware polls sensors at configurable intervals and renders live telemetry on a 128×64 OLED display.
- Custom framebuffer management ensures flicker-free UI updates even under high-frequency sampling.
- Display layers are logically separated: status bar, data readout, and alert overlay render independently.

STM32 固件以可配置间隔轮询传感器，将实时遥测数据渲染至 128×64 OLED 屏幕。自定义帧缓冲管理确保高频采样下无闪烁 UI 刷新，显示层逻辑分离：状态栏、数据读出区与告警叠加层独立渲染。

---

### ⚙️ High-Performance C++ Backend Processing
**基于 C++ 的高性能后端处理**

- Modular C++17 service handles serial ingestion, data validation, anomaly flagging, and write-through to MySQL.
- Lock-free ring buffers decouple the I/O thread from the processing thread, sustaining throughput under burst sampling conditions.
- Clean separation of concerns: `DataIngestor`, `DataFilter`, `StorageEngine`, and `AIQueryDispatcher` as independent modules.

模块化 C++17 服务处理串口接入、数据校验、异常标记与直写 MySQL。无锁环形缓冲区解耦 I/O 线程与处理线程，在突发采样条件下维持吞吐量。职责清晰分离：`DataIngestor`、`DataFilter`、`StorageEngine`、`AIQueryDispatcher` 作为独立模块。

---

### 🗄️ MySQL Historical Data Management & Query Optimization
**MySQL 历史数据管理与查询优化**

- Time-series sensor records stored in normalized schemas with composite indexes on `(device_id, timestamp)`.
- Sliding-window aggregation queries pre-computed via stored procedures to accelerate dashboard and LLM context retrieval.
- Automated retention policies archive cold data beyond configurable thresholds.

传感器时序记录以规范化模式存储，在 `(device_id, timestamp)` 上建立复合索引。滑动窗口聚合查询通过存储过程预计算，加速仪表板与 LLM 上下文检索。自动化保留策略归档超出阈值的冷数据。

---

### 🤖 Local LLM-Driven Intelligent Decision Support
**本地大模型（LLM）驱动的智能决策支持**

- Qwen2 runs fully offline via Ollama; no external API calls, no telemetry exfiltration.
- The C++ backend constructs structured prompts from recent sensor windows and injects them into the model's context.
- Inference results — diagnostics, threshold breach explanations, and maintenance recommendations — are logged and optionally relayed back to the OLED display.

Qwen2 通过 Ollama 完全离线运行，零外部 API 调用，零遥测数据外泄。C++ 后端从最近传感器窗口构造结构化提示词并注入模型上下文。推理结果——诊断信息、阈值超限解释与维护建议——被记录日志，并可选择性回传至 OLED 显示屏。

---

## 🏗️ System Architecture | 系统架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        SENSING LAYER  感知层                             │
│                                                                          │
│   [Temperature]  [Humidity]  [Pressure]  [Custom Sensor N]              │
│         │              │           │              │                      │
│         └──────────────┴───────────┴──────────────┘                     │
│                                  │                                       │
│                         ┌────────▼────────┐                             │
│                         │  STM32 MCU      │  ← Firmware (C / HAL)       │
│                         │  ┌───────────┐  │                             │
│                         │  │ OLED 12864│  │  ← Real-time display        │
│                         │  └───────────┘  │                             │
│                         └────────┬────────┘                             │
└──────────────────────────────────┼──────────────────────────────────────┘
                                   │  Serial / UART
                                   │
┌──────────────────────────────────▼──────────────────────────────────────┐
│                    EDGE PROCESSING LAYER  边缘处理层                     │
│                       (Ubuntu Linux Server)                              │
│                                                                          │
│   ┌─────────────────────────────────────────────────────────────────┐   │
│   │                    C++ Backend Service                           │   │
│   │                                                                  │   │
│   │  DataIngestor → DataFilter → StorageEngine → AIQueryDispatcher  │   │
│   │       │              │             │                │            │   │
│   │  [Serial RX]   [Validation]   [Write-through]  [Prompt Build]   │   │
│   └──────────────────────┬──────────────┬────────────────┬──────────┘   │
│                          │              │                │               │
│              ┌───────────▼──┐    ┌──────▼──────┐  ┌────▼──────────┐    │
│              │  Ring Buffer │    │    MySQL     │  │  Ollama/Qwen2 │    │
│              │  (Lock-free) │    │  (InnoDB)    │  │  (Local LLM)  │    │
│              └──────────────┘    └─────────────┘  └───────────────┘    │
│                                                           │              │
│                                              ┌────────────▼───────────┐ │
│                                              │  Inference Result Log  │ │
│                                              │  → OLED Feedback (opt) │ │
│                                              └────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

**Data Flow Summary | 数据流摘要**

| Stage | Component | Responsibility |
|---|---|---|
| Acquisition | STM32 + Sensors | Raw ADC/digital sampling, local display |
| Transport | UART / Serial | Framed packet transmission |
| Ingestion | C++ `DataIngestor` | Deserialization, CRC validation |
| Filtering | C++ `DataFilter` | Outlier rejection, unit conversion |
| Persistence | MySQL (InnoDB) | Time-series storage, indexed queries |
| Intelligence | Ollama + Qwen2 | Contextual analysis, recommendations |
| Feedback | OLED / Log File | Human-readable output |

---

## 🧮 Algorithm & Logic | 算法与核心逻辑

### Low-Level Driver & Data Filtering (C++) | 底层驱动与数据过滤（C++）

The firmware layer implements a **DMA-assisted ADC sampling loop** to minimize CPU blocking during sensor reads. Raw samples pass through a **sliding median filter** (window size configurable at compile-time via template parameter) before serialization:

固件层实现 **DMA 辅助 ADC 采样循环**，最小化传感器读取期间的 CPU 阻塞。原始采样值在序列化前通过**滑动中值滤波器**（窗口大小通过模板参数在编译期可配置）：

```cpp
// Compile-time configurable median filter — zero heap allocation
template <typename T, std::size_t N>
class MedianFilter {
    std::array<T, N> _window{};
    std::size_t      _head = 0;
    bool             _full = false;

public:
    T push(T sample) {
        _window[_head] = sample;
        _head = (_head + 1) % N;
        if (_head == 0) _full = true;

        auto buf = _full ? _window
                         : std::array<T, N>(_window.begin(),
                                            _window.begin() + _head);
        std::nth_element(buf.begin(), buf.begin() + buf.size() / 2, buf.end());
        return buf[buf.size() / 2];
    }
};
```

On the backend, an **IQR-based anomaly detector** flags samples that deviate beyond 1.5× the interquartile range of the trailing 60-second window, writing flagged records to a separate `anomaly_events` table for LLM context injection.

在后端，**基于 IQR 的异常检测器**标记偏离过去 60 秒窗口四分位距 1.5 倍以上的采样值，将标记记录写入独立的 `anomaly_events` 表，供 LLM 上下文注入使用。

---

### Local LLM Deployment & Quantization | 本地大模型部署与量化

> **Design principle:** Inference must complete within the sensor sampling interval to avoid pipeline backpressure.
>
> **设计原则：** 推理必须在传感器采样间隔内完成，以避免流水线背压。

- Qwen2 is served via **Ollama** with the `Q4_K_M` quantization preset, reducing the model footprint to ~4.5 GB while preserving >95% of full-precision benchmark accuracy.
- The C++ `AIQueryDispatcher` communicates with the Ollama REST API over `localhost`, constructing prompts that embed a JSON-serialized sensor summary and the most recent anomaly event descriptions.
- Response parsing extracts structured fields (`severity`, `diagnosis`, `recommendation`) using a lightweight JSON parser, avoiding dependency on heavy third-party libraries.

Qwen2 通过 **Ollama** 以 `Q4_K_M` 量化预设部署，将模型体积缩减至约 4.5 GB，同时保留 >95% 的全精度基准精度。C++ `AIQueryDispatcher` 通过 `localhost` REST API 与 Ollama 通信，构造嵌入 JSON 序列化传感器摘要与最近异常事件描述的提示词。响应解析通过轻量级 JSON 解析器提取结构化字段（`severity`、`diagnosis`、`recommendation`），避免引入重量级第三方库。

```bash
# Example Ollama model pull & serve
ollama pull qwen2:7b-instruct-q4_K_M
ollama serve  # Binds to localhost:11434 by default
```

---

## 🛠️ Tech Stack | 技术栈

| Layer | Technology | Version / Notes |
|---|---|---|
| **MCU** | STM32 (Cortex-M series) | HAL + CubeMX codegen |
| **Display** | SSD1306 OLED 128×64 | SPI/I²C interface |
| **Backend Language** | C++ | C++17, GCC 12 |
| **Build System** | CMake | 3.22+ |
| **OS** | Ubuntu Linux | 22.04 LTS |
| **Database** | MySQL | 8.0, InnoDB engine |
| **LLM Runtime** | Ollama | Latest stable |
| **LLM Model** | Qwen2 | 7B-Instruct, Q4_K_M quant |
| **Serial Comm** | UART / USB-CDC | Custom framing protocol |
| **Version Control** | Git | GitHub hosted |

---

## 🚀 Installation & Setup | 环境配置

### Prerequisites | 前置条件

- Ubuntu 22.04 LTS (bare-metal or VM)
- `gcc-12`, `cmake >= 3.22`, `libmysqlclient-dev`
- MySQL 8.0 server running locally
- Ollama installed and Qwen2 model pulled (see above)

---

### Step 1 — Clone & Build Backend | 克隆与编译后端

```bash
git clone https://github.com/<your-username>/edge-iot-monitor.git
cd edge-iot-monitor/backend

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Step 2 — Initialize Database | 初始化数据库

```bash
mysql -u root -p < ../sql/schema.sql
# schema.sql creates: sensor_db, sensor_readings, anomaly_events tables
```

```sql
-- Verify tables
USE sensor_db;
SHOW TABLES;
-- Expected: sensor_readings, anomaly_events, device_registry
```

### Step 3 — Configure the Service | 配置服务

```bash
cp ../config/config.example.yaml ../config/config.yaml
# Edit config.yaml:
#   serial.port:    /dev/ttyUSB0   (adjust to your device)
#   mysql.host:     127.0.0.1
#   mysql.db:       sensor_db
#   ollama.endpoint: http://127.0.0.1:11434
#   ollama.model:   qwen2:7b-instruct-q4_K_M
```

### Step 4 — Run | 启动服务

```bash
# Terminal 1: ensure Ollama is serving
ollama serve

# Terminal 2: start the backend
./build/edge_iot_monitor --config config/config.yaml
```

### Step 5 — Flash STM32 Firmware | 烧录 STM32 固件

```bash
# Using STM32CubeProgrammer CLI
STM32_Programmer_CLI -c port=SWD -w firmware/edge_sensor.elf -v -rst
```

---

## 🔭 Future Roadmap | 未来规划

- [ ] **Advanced Signal Processing** — Integrate FFT-based frequency-domain analysis for vibration sensor data; implement Kalman filtering for multi-sensor fusion to improve state estimation accuracy under noisy conditions.
  
  **高级信号处理** —— 集成基于 FFT 的频域分析用于振动传感器数据；实现多传感器融合的卡尔曼滤波，在噪声条件下提升状态估计精度。

- [ ] **MQTT Protocol Support** — Extend the communication layer to support MQTT pub/sub for multi-device deployments, enabling fleet-level monitoring from a single backend instance.
  
  **MQTT 协议支持** —— 扩展通信层以支持 MQTT 发布/订阅，用于多设备部署，实现从单一后端实例进行机群级监控。

- [ ] **Web Dashboard** — Lightweight embedded web server (e.g., cpp-httplib) serving a real-time visualization dashboard, replacing the OLED as the primary human interface for desktop access.
  
  **Web 仪表板** —— 轻量级嵌入式 Web 服务器（如 cpp-httplib）提供实时可视化仪表板，替代 OLED 作为桌面访问的主要人机界面。

- [ ] **Federated Learning Prototype** — Explore on-device fine-tuning of a lightweight model (e.g., Qwen2-0.5B) using anomaly event feedback, moving toward adaptive, self-improving edge intelligence.
  
  **联邦学习原型** —— 探索使用异常事件反馈对轻量级模型（如 Qwen2-0.5B）进行设备端微调，向自适应、自进化的边缘智能演进。

- [ ] **Hardware Watchdog & OTA Firmware Update** — Implement a hardware watchdog on the STM32 side and an over-the-air firmware update mechanism via the Linux backend.
  
  **硬件看门狗与 OTA 固件更新** —— 在 STM32 端实现硬件看门狗，并通过 Linux 后端实现空中固件更新机制。

---

## 📄 License

This project is licensed under the **MIT License**. See the [LICENSE](./LICENSE) file for details.

---

<p align="center">
  Built with precision for the intersection of embedded systems, edge computing, and on-premise AI.<br/>
  <em>为嵌入式系统、边缘计算与本地 AI 的交汇处，精心构建。</em>
</p>
