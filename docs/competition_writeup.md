# Competition Writeup

This document contains wording that can be used in the competition proposal, report, or presentation.

## Project Name

Cloud-Based Intelligent Environment Monitoring and Alerting System

中文名称：

```text
云端智能环境监测与告警系统
```

## One-Sentence Summary

本项目基于 ESP32-S3、SHT30、OLED、蜂鸣器和华为云 IoTDA，构建了一套“端侧采集、云端接入、大模型分析、平台下发、设备告警”的智能环境监测闭环系统。

## Background

传统温湿度监测系统通常依赖固定阈值触发告警，只能判断当前数值是否超限，难以解释异常原因，也难以根据趋势提前判断风险。本项目引入云端大语言模型分析能力，让系统不仅能监测温湿度，还能结合最近一段时间的数据变化进行风险分析、原因解释和处理建议生成。

## Technical Route

设备端采用 ESP32-S3 作为主控，连接 SHT30 温湿度传感器、OLED 显示屏和蜂鸣器。ESP32-S3 通过 Wi-Fi 接入华为云 IoTDA，并按照 IoTDA 官方 MQTT/MQTTS 设备侧接口上报属性数据。

华为云 IoTDA 负责设备接入、产品模型、属性上报、设备影子、数据流转和命令下发。云端分析服务接收 IoTDA 转发的数据，维护最近一段时间的温湿度趋势，并调用云端 LLM API 进行风险分析。

当 LLM 判断存在高温、高湿、凝露、通风不足或环境突变风险时，云端服务将分析结果转换为 IoTDA 官方设备命令，通过平台下发到 ESP32-S3。设备接收命令后控制蜂鸣器和 OLED 显示，实现物理告警。

## System Closed Loop

```text
ESP32-S3 采集数据
  -> 华为云 IoTDA 属性上报
  -> 云端分析服务
  -> 云端 LLM 风险推理
  -> IoTDA 命令下发
  -> ESP32-S3 buzzer 告警
```

## Innovation Points

1. 云端 LLM 不用于聊天，而是作为环境风险分析模块。
2. 系统根据最近一段时间温湿度趋势判断风险，不只依赖单点阈值。
3. LLM 输出异常原因解释、风险等级和处理建议，提升系统可解释性。
4. ESP32-S3 不保存 LLM API key，降低密钥泄露风险。
5. 华为云 IoTDA 作为标准云端设备接入和命令通道，提高方案规范性。
6. 本地 Ollama 可作为云端 LLM 不可用时的备用分析引擎。
7. 规则阈值作为最终安全兜底，避免完全依赖大模型输出。

## Safety and Security Design

- ESP32-S3 只负责采集和执行，不直接调用 LLM API。
- LLM API key 只保存在云端环境变量中。
- IoTDA 设备属性上报使用官方 Topic 和官方 JSON 结构。
- IoTDA 命令下发使用官方命令格式。
- LLM 输出必须经过云端服务校验后才能转换为设备命令。
- 蜂鸣器控制命令采用白名单参数，避免任意 GPIO 控制。
- 本地 Ollama 只运行在服务器、边缘网关或演示电脑上，不运行在 ESP32-S3 上。

## Recommended Presentation Wording

```text
本项目设计了一套基于 ESP32-S3、华为云 IoTDA 和云端大模型分析服务的智能环境监测与告警系统。终端设备通过 SHT30 传感器实时采集温湿度数据，并按照华为云 IoTDA 官方设备属性上报格式，通过 MQTT/MQTTS 将数据上报至云平台。IoTDA 负责设备接入、产品模型管理、设备影子、数据流转和云端命令下发。

云端分析服务接收 IoTDA 转发的数据后，维护最近一段时间的温湿度趋势，并调用云端大语言模型 API 对环境风险进行分析。LLM 根据趋势数据输出风险等级、异常原因解释、处理建议以及是否建议触发告警。云端服务对 LLM 输出进行结构化校验，并结合规则阈值进行安全兜底，最终通过 IoTDA 命令下发接口控制 ESP32-S3 的 buzzer/GPIO。

系统形成了从数据采集、云端接入、智能分析、平台控制到本地告警的完整闭环。与传统阈值告警相比，本项目具备趋势理解、风险解释和智能建议能力，同时将 API key 和推理服务保留在云端，提升了系统的安全性、可扩展性和比赛展示价值。
```

## Backup Reasoning Wording

```text
为提高系统可靠性，本项目设计了多级分析机制：云端 LLM API 作为主要风险推理引擎，本地 Ollama 作为云端 API 不可用时的备用分析引擎，规则阈值作为最终安全兜底。该机制使系统在网络异常、API 额度不足或云端模型服务故障时，仍能保持基础分析和告警能力。
```
