# Edge IoT Monitor

一个边缘侧物联网监控原型：ESP32-S3 采集温湿度数据，通过 HTTP 上报到本地 C++ 后端，后端写入 SQLite、做 IQR 异常检测，并调用 LLM 生成传感器窗口分析（云端 DeepSeek 为主，本机 Ollama 备用）。

当前仓库重点是“本地可跑通”的端到端链路：模拟传感器或 ESP32-S3 → C++ HTTP 接入 → SQLite → 异常检测 → LLM 分析（DeepSeek / Ollama）。云端同步代码已预留接口，实际发送逻辑仍是 TODO。

## 当前状态

| 模块 | 状态 | 说明 |
|---|---|---|
| 固件 | 可构建原型 | PlatformIO + Arduino，支持 WiFi、SHT30、OLED、HTTP 上报 |
| 后端 HTTP 服务 | 可构建运行 | `edge_server`，接收 `POST /api/ingest` |
| 本地导入工具 | 可构建运行 | `edge_ingest`，从标准输入读取 JSON 行并写入 SQLite |
| 数据库存储 | 可用 | SQLite schema 位于 `sql/schema.sql` |
| 异常检测 | 可用 | 按设备和传感器类型维护滑动窗口，使用 IQR 判定异常 |
| AI 分析 | 可用 | 云端 DeepSeek 为主（需 API key），失败自动降级本机 Ollama `qwen2.5:3b`，结果写入 `analysis_log` |
| 云同步 | 占位 | `CloudSync` 已接入流水线，华为云发送逻辑未实现 |

## 项目结构

```text
edge-iot-monitor/
├── backend/                 # C++17 后端与本地导入工具
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp         # edge_server 入口
│       ├── ingest_stdin.cpp # edge_ingest 入口
│       ├── DataIngestor.*   # HTTP 接入、JSON 解析、数据校验
│       ├── DataFilter.*     # IQR 异常检测
│       ├── StorageEngine.*  # SQLite 初始化、插入、查询
│       ├── AIQueryDispatcher.*
│       ├── CloudSync.*      # 云同步占位
│       └── ConfigLoader.*   # config/config.yaml 加载
├── config/
│   └── config.example.yaml  # 后端示例配置
├── docs/
│   └── architecture.md      # 架构说明
├── firmware/                # ESP32-S3 PlatformIO 工程
│   ├── platformio.ini
│   └── src/
├── scripts/
│   ├── simulate_sensor.py   # 模拟传感器，可 HTTP POST 或输出 JSON 行
│   └── test_ollama.py       # Ollama 连通性和推理测试
└── sql/
    └── schema.sql           # SQLite 表结构
```

## 数据流

```text
ESP32-S3 或 scripts/simulate_sensor.py
  -> POST /api/ingest
  -> DataIngestor
      -> JSON 解析与范围校验
      -> 拆分为 temperature / humidity 传感器行
  -> DataFilter
      -> 每个 device_id + sensor_type 独立滑动窗口
      -> IQR 异常检测
  -> StorageEngine
      -> sensor_readings / anomaly_events / analysis_log / sync_status
  -> AIQueryDispatcher
      -> 每 N 条记录触发一次 LLM 分析（后台线程执行，不阻塞上报请求）
      -> 首选云端 DeepSeek，失败自动降级本机 Ollama
  -> CloudSync
      -> 当前仅占位，未真正发送云端请求
```

当前 HTTP 载荷支持 `temperature` 和 `humidity`。模拟器也会生成 `pressure`，但 HTTP 服务目前不会存储 pressure；本地 `edge_ingest` 工具支持把 pressure 写入数据库。

示例载荷：

```json
{
  "device_id": "esp32s3-001",
  "timestamp": 1700000000,
  "temperature": 24.3,
  "humidity": 58.7
}
```

## 依赖

后端：

- CMake 3.20+
- C++17 编译器
- SQLite 源码已随仓库放在 `backend/src/sqlite3.c`
- 构建 `edge_server` 时需要 `yaml-cpp` 和 OpenSSL（DeepSeek HTTPS 调用）
- 云端 AI 分析需要 DeepSeek API key（写入 `config/config.yaml` 或设置环境变量 `DEEPSEEK_API_KEY`）
- 本地降级分析需要本机运行 Ollama，并拉取 `qwen2.5:3b`

固件：

- PlatformIO
- ESP32-S3 DevKitC-1
- Arduino framework
- SHT30 传感器
- OLED 显示模块

## 快速开始：后端 HTTP 服务

1. 准备配置文件：

```bash
cp config/config.example.yaml config/config.yaml
```

注意 `config.example.yaml` 默认启用了设备 allowlist。模拟器默认设备是 `esp32-s3-sim-001`，可以选择：

- 把 `esp32-s3-sim-001` 加到 `config/config.yaml` 的 `devices.allowlist`
- 或运行模拟器时指定已允许的设备，例如 `--device esp32s3-001`
- 或临时把 allowlist 改为空列表 `[]`

2. 配置 AI 分析（可选）：

云端 DeepSeek（首选）——在 `config/config.yaml` 的 `deepseek.api_key` 填入 key，或：

```bash
export DEEPSEEK_API_KEY=sk-...
```

本地 Ollama 备用（DeepSeek 不可用时自动降级）：

```bash
ollama pull qwen2.5:3b
```

3. 构建 HTTP 服务：

```bash
cmake -S . -B build-cmake -DEDGE_BUILD_SERVER=ON
cmake --build build-cmake
```

4. 运行后端：

```bash
./build-cmake/backend/edge_server
```

服务启动后会监听：

- `GET /health`
- `POST /api/ingest`

5. 用模拟器发送数据：

```bash
python3 scripts/simulate_sensor.py --device esp32s3-001 --count 10
```

## 快速开始：无需 HTTP 的本地写库测试

默认构建会生成 `edge_ingest`，它适合快速验证 SQLite 写入：

```bash
cmake -S . -B build-cmake
cmake --build build-cmake
python3 scripts/simulate_sensor.py --stdout --count 10 | ./build-cmake/backend/edge_ingest --db sensor.db
```

这条链路不会调用 HTTP、异常检测或 Ollama，只验证 JSON 行解析和 SQLite 写入。

## 测试 Ollama

```bash
python3 scripts/test_ollama.py --model qwen2.5:3b
```

如果只是检查 Ollama 是否在线：

```bash
python3 scripts/test_ollama.py --check-only
```

## 固件配置

固件位于 `firmware/`，使用 PlatformIO：

```bash
cd firmware
pio run
```

首次烧录前需要基于示例创建本地配置：

```bash
cp src/config.example.h src/config.h
```

然后填写：

- `WIFI_SSID`
- `WIFI_PASS`
- `SERVER_URL`，例如 `http://192.168.x.x:8080/api/ingest`

`src/config.h` 属于本地敏感配置，不应提交。

## 数据库

数据库表结构由 `sql/schema.sql` 维护，`StorageEngine::init()` 会在启动时执行该 schema。

主要表：

- `sensor_readings`：每条传感器指标一行
- `anomaly_events`：异常检测结果
- `analysis_log`：LLM prompt 和 response
- `sync_status`：云同步进度占位

## 配置说明

后端启动时读取 `config/config.yaml`。如果文件不存在，会使用代码里的默认值。

常用配置：

- `server.port`：HTTP 服务端口，默认 `8080`
- `sqlite.db_path`：SQLite 文件路径，默认 `sensor.db`
- `deepseek.enabled` / `deepseek.model` / `deepseek.api_key`：云端首选推理后端（key 也可用 `DEEPSEEK_API_KEY` 环境变量提供，优先级更高）
- `ollama.host` / `ollama.port` / `ollama.model`：本地备用模型服务
- `filter.window_seconds` / `filter.iqr_multiplier` / `filter.min_window_samples`：异常检测参数
- `ai.trigger_every_n_records` / `ai.max_window_records`：AI 分析触发频率和窗口大小
- `devices.allowlist`：允许接入的设备 ID
- `cloud.*`：华为云 IoTDA 预留配置，当前发送逻辑未完成

## License

MIT
