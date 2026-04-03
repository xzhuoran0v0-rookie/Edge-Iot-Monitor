# Contributing Guide | 贡献指南

Thank you for your interest in this project. This document outlines the development workflow, coding standards, and module ownership for the **Edge-Intelligence IoT Monitoring & Analysis System**.

---

## Project Structure | 项目结构

```
edge-iot-monitor/
│
├── firmware/               # ESP32-S3 端固件 (ESP-IDF / Arduino)
│   ├── src/                #   业务逻辑源码
│   └── include/            #   头文件
│
├── backend/                # Linux C++ 后端服务
│   ├── src/                #   各模块实现
│   │   ├── main.cpp
│   │   ├── DataIngestor.cpp
│   │   ├── DataFilter.cpp
│   │   ├── StorageEngine.cpp
│   │   └── AIQueryDispatcher.cpp
│   └── include/            #   对应头文件
│
├── sql/                    # 数据库 Schema 与迁移脚本
│   └── schema.sql
│
├── config/                 # 配置文件模板（不含真实凭据）
│   └── config.example.yaml
│
├── scripts/                # 辅助脚本（模拟数据、测试、部署）
│   ├── simulate_sensor.py  #   传感器数据模拟器（开发阶段用）
│   └── test_ollama.py      #   Ollama 接口测试
│
└── docs/                   # 详细技术文档
    └── architecture.md
```

---

## Development Phases | 开发阶段

### Phase 1 — Backend & AI Pipeline（推荐首先完成）
> 目标：无需硬件，用模拟数据跑通完整后端链路

- [ ] MySQL schema 建表
- [ ] C++ `DataIngestor` 串口/HTTP 接收模块
- [ ] C++ `StorageEngine` MySQL 写入模块
- [ ] Ollama + Qwen2 本地部署验证
- [ ] C++ `AIQueryDispatcher` 提示词构造与推理调用
- [ ] `scripts/simulate_sensor.py` 模拟数据注入

### Phase 2 — Firmware（ESP32-S3）
> 目标：真实传感器数据替换模拟脚本

- [ ] ESP-IDF 环境搭建
- [ ] 传感器驱动（I²C / SPI）
- [ ] WiFi 连接 + HTTP POST 上报
- [ ] OLED 128×64 显示逻辑

### Phase 3 — Closed Loop
> 目标：LLM 结果回传到 OLED，系统闭环

- [ ] 后端下行接口（HTTP GET / MQTT）
- [ ] 固件接收并渲染分析结果
- [ ] 端到端集成测试

---

## Coding Standards | 编码规范

### C++ (Backend)
- 标准：**C++17**
- 编译器：GCC 12+，开启 `-Wall -Wextra`
- 命名：类名 `PascalCase`，函数/变量 `camelCase`，常量 `UPPER_SNAKE_CASE`
- 每个模块对应独立的 `.cpp` + `.hpp` 文件对
- 禁止裸指针管理资源，使用 `std::unique_ptr` / `std::shared_ptr`

### Firmware (ESP32-S3)
- 框架：ESP-IDF v5.x 或 Arduino Core for ESP32
- 传感器驱动封装为独立 `.h/.cpp` 模块
- 所有 WiFi 凭据通过 `menuconfig` 或环境变量注入，**不硬编码**

### Git Commit 规范
使用语义化提交信息：

```
feat(backend): add IQR-based anomaly detector
fix(firmware): correct OLED I2C address to 0x3C
docs(arch): update data flow diagram
chore(sql): add index on (device_id, timestamp)
```

---

## Environment Setup Quick Reference | 环境速查

详细步骤见 [README.md — Installation & Setup](../README.md#-installation--setup--环境配置)。

```bash
# 1. 安装依赖
sudo apt update && sudo apt install -y cmake gcc g++ libmysqlclient-dev

# 2. 启动 Ollama
ollama serve

# 3. 用模拟器代替硬件测试
python3 scripts/simulate_sensor.py

# 4. 编译后端
cd backend && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

---

## Questions | 有问题？

请在 GitHub Issues 中提交，标注相应的模块标签（`firmware` / `backend` / `database` / `ai`）。
