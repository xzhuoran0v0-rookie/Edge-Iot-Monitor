# System Architecture

This project is designed as a cloud-based intelligent environment monitoring and alerting system for a competition demo.

The main idea is simple:

```text
ESP32-S3 handles sensing and actuation.
Huawei Cloud IoTDA handles device access and command transport.
The cloud analysis service handles LLM reasoning and alert decisions.
```

## Target Architecture

```text
+-----------------------------+
| ESP32-S3                    |
| - SHT30 temperature/humidity |
| - OLED status display        |
| - Buzzer / GPIO alert        |
| - Wi-Fi network              |
+-------------+---------------+
              |
              | MQTT/MQTTS
              | Official IoTDA property report
              v
+-----------------------------+
| Huawei Cloud IoTDA           |
| - Device access              |
| - Product model              |
| - Property report            |
| - Device shadow              |
| - Data forwarding            |
| - Command downlink           |
+-------------+---------------+
              |
              | Rule-based data forwarding
              v
+-----------------------------+
| Cloud Analysis Service       |
| FunctionGraph / ECS /        |
| Cloud Container              |
| - Receive IoTDA data         |
| - Keep recent data window    |
| - Call cloud LLM API         |
| - Fallback to local Ollama   |
| - Validate reasoning output  |
| - Send IoTDA commands        |
+-------------+---------------+
              |
              | HTTPS API
              v
+-----------------------------+
| Reasoning Engines            |
| Primary: Cloud LLM API        |
| Backup: Local Ollama          |
| Final fallback: rules         |
+-----------------------------+
```

## Responsibility Split

| Layer | Responsibility |
|---|---|
| ESP32-S3 | Collect SHT30 data, report to IoTDA, receive commands, control OLED and buzzer |
| Huawei Cloud IoTDA | Device authentication, MQTT/MQTTS transport, product model, property report, command downlink |
| Cloud analysis service | Trend window, LLM prompt, risk decision, validation, command generation |
| Cloud LLM | Risk level, abnormal reason, trend explanation, suggestions |
| Local Ollama backup | Backup reasoning engine when cloud LLM is unavailable |
| Rule engine | Final safety fallback for extreme temperature or humidity |

## Closed Loop

```text
1. ESP32-S3 reads SHT30 temperature and humidity.
2. ESP32-S3 reports properties to IoTDA using the official topic and JSON structure.
3. IoTDA forwards data to the cloud analysis service.
4. The cloud service stores the recent data window.
5. The cloud service calls a cloud LLM API for risk reasoning.
6. If the cloud LLM fails, the service can fall back to local Ollama.
7. If all LLM reasoning fails, rule thresholds still provide safety fallback.
8. The cloud service converts the decision into an IoTDA command.
9. IoTDA downlinks the command to ESP32-S3.
10. ESP32-S3 controls buzzer/GPIO and updates OLED.
```

## Why This Architecture Fits the Competition

- The device remains small, stable, and easy to explain.
- IoTDA provides a real cloud IoT access layer instead of a private local-only protocol.
- LLM reasoning is used for analysis, not chatting.
- API keys stay in the cloud, not in firmware.
- Ollama is still useful as a local backup and demo resilience feature.
- The final system has a clear closed loop from sensing to cloud analysis to physical alert.

## Related Documents

- [hardware.md](hardware.md)
- [cloud_iotda.md](cloud_iotda.md)
- [llm_reasoning.md](llm_reasoning.md)
- [backend_service.md](backend_service.md)
- [data_flow.md](data_flow.md)
- [competition_writeup.md](competition_writeup.md)
