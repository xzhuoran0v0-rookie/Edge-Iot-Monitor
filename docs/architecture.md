# System Architecture | 系统架构详解

## Overview | 概述

本系统采用三层架构：**感知层**（ESP32-S3 + 传感器）、**边缘处理层**（Linux C++ 后端 + MySQL + Ollama）、**展示层**（OLED + 日志）。所有计算和 AI 推理均在本地完成，无外部云依赖。

---

## Layer Breakdown | 分层详解

### Layer 1 — Sensing Layer | 感知层

| Component | Role |
|---|---|
| ESP32-S3-N16R8 | 主控 MCU，16MB Flash，8MB PSRAM |
| 温湿度传感器 (e.g. SHT31) | I²C 接口，采集环境数据 |
| OLED 128×64 (SSD1306) | 实时本地显示 |
| WiFi (802.11 b/g/n) | 数据上报至后端 |

**固件数据流：**
```
传感器 (I²C polling)
    → MedianFilter<float, 8>  // 去抖动，模板化实现
    → JSON 序列化
    → HTTP POST → 后端 /api/ingest
    → OLED 渲染当前读数
```

---

### Layer 2 — Edge Processing Layer | 边缘处理层

运行在本地 Ubuntu Linux 服务器（或 VMware VM）上，由四个 C++ 模块组成：

```
HTTP Server (接收固件 POST)
    │
    ▼
DataIngestor          — 反序列化 JSON，CRC/范围校验
    │
    ▼
DataFilter            — IQR 异常检测，单位换算
    │
    ├──► StorageEngine ──► MySQL sensor_db
    │         │
    │         │ (每 N 条触发一次 AI 分析)
    │         ▼
    └──► AIQueryDispatcher
              │
              │  构造结构化 Prompt（含最近窗口数据 + 异常事件）
              ▼
         Ollama REST API (localhost:11434)
              │
              ▼
         Qwen2-7B-Instruct (Q4_K_M 量化)
              │
              ▼
         解析响应 → 写入 analysis_log 表
              │
              ▼ (可选)
         下行推送 → ESP32-S3 OLED 显示分析摘要
```

#### Module Responsibilities | 模块职责

**`DataIngestor`**
- 监听 HTTP `/api/ingest` 端点
- 解析 JSON payload：`{ "device_id", "timestamp", "temperature", "humidity", ... }`
- 校验字段完整性与数值范围，拒绝非法数据包

**`DataFilter`**
- 维护每个 `device_id` 的滑动窗口（60秒）
- 基于 IQR（四分位距）检测统计异常值
- 异常记录写入 `anomaly_events` 表，正常数据写入 `sensor_readings`

**`StorageEngine`**
- 封装所有 MySQL 操作，使用连接池（避免频繁建连开销）
- 批量写入优化：积累 N 条后执行单次 `INSERT ... VALUES (...),(...),...`
- 暴露 `queryRecentWindow(device_id, seconds)` 供 AIQueryDispatcher 调用

**`AIQueryDispatcher`**
- 定时（或事件驱动）从 StorageEngine 拉取最近数据窗口
- 构造 Prompt 并通过 HTTP POST 调用 `localhost:11434/api/generate`
- 解析响应中的 `severity` / `diagnosis` / `recommendation` 字段
- 结果写入 `analysis_log` 表，并可选回传至设备

---

### Layer 3 — Presentation Layer | 展示层

- **OLED 本地显示**：实时传感器读数 + 最新告警状态
- **分析日志**：MySQL `analysis_log` 表，可通过 SQL 直接查询
- **终端输出**：后端服务标准输出结构化日志（便于调试）

---

## Database Schema | 数据库结构

```sql
-- 三张核心表，详见 sql/schema.sql

sensor_readings    -- 每条传感器采样记录
anomaly_events     -- 异常检测标记记录
analysis_log       -- LLM 推理结果存档
```

关键索引设计：
- `sensor_readings(device_id, timestamp)` 复合索引 → 支持时序范围查询
- `anomaly_events(device_id, detected_at)` → 支持异常回溯

---

## Communication Protocol | 通信协议

### ESP32-S3 → Linux Backend

```
POST /api/ingest HTTP/1.1
Content-Type: application/json

{
  "device_id":   "esp32-s3-001",
  "timestamp":   1720000000,
  "temperature": 27.4,
  "humidity":    68.2,
  "pressure":    1013.5,
  "checksum":    "a3f2"
}
```

### Linux Backend → Ollama

```
POST http://localhost:11434/api/generate
{
  "model": "qwen2:7b-instruct-q4_K_M",
  "prompt": "...(结构化传感器摘要 + 异常事件描述)...",
  "stream": false
}
```

---

## Key Design Decisions | 关键设计决策

| Decision | Rationale |
|---|---|
| 本地 LLM 而非云 API | 数据不出私有网络，无费用，无延迟抖动 |
| Q4_K_M 量化 | 在 ~4.5GB 内存占用下保留 >95% 精度 |
| C++ 后端而非 Python | 更低的内存占用与更可预测的延迟 |
| ESP32-S3 而非 STM32 | 内置 WiFi，简化端到云通信链路 |
| InnoDB 引擎 | 支持事务、行级锁，适合并发写入场景 |
