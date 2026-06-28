# Competition Writeup

This document contains wording that can be used in the competition proposal, report, or presentation.

## Project Name

Cloud-Based Intelligent Environment Monitoring and Alerting System

## One-Sentence Summary

Built with an ESP32-S3, SHT30, OLED, buzzer, and Huawei Cloud IoTDA, this project creates a closed-loop intelligent environment monitoring system covering edge data collection, cloud access, LLM analysis, platform command delivery, and device alerts.

## Background

Traditional temperature and humidity monitoring systems usually trigger alarms with fixed thresholds. They can identify a value outside a limit, but struggle to explain the cause or predict risk from recent trends. This project adds cloud LLM analysis so the system can monitor temperature and humidity, evaluate recent changes, explain likely causes, and recommend actions.

## Technical Route

The device uses an ESP32-S3 as its controller and connects to an SHT30 temperature and humidity sensor, an OLED display, and a buzzer. It connects to Huawei Cloud IoTDA over Wi-Fi and reports properties through the official IoTDA MQTT/MQTTS device interface.

Huawei Cloud IoTDA handles device access, product models, property reporting, device shadows, data forwarding, and command delivery. The cloud analysis service receives data forwarded by IoTDA, maintains recent temperature and humidity trends, and calls a cloud LLM API for risk analysis.

When the LLM identifies risks such as high temperature, high humidity, condensation, poor ventilation, or a sudden environmental change, the cloud service converts the result into an official IoTDA device command and sends it to the ESP32-S3. The device then controls the buzzer and OLED to produce a physical alert.

## System Closed Loop

```text
ESP32-S3 data collection
  -> Huawei Cloud IoTDA property report
  -> Cloud analysis service
  -> Cloud LLM risk reasoning
  -> IoTDA command delivery
  -> ESP32-S3 buzzer alert
```

## Innovation Points

1. The cloud LLM acts as an environment risk analysis module rather than a chatbot.
2. The system evaluates recent temperature and humidity trends instead of relying only on individual threshold readings.
3. The LLM explains abnormal conditions, assigns a risk level, and recommends actions, improving system explainability.
4. The ESP32-S3 does not store the LLM API key, reducing the risk of credential exposure.
5. Huawei Cloud IoTDA provides standardized device access and command delivery.
6. A local Ollama model can serve as a backup analysis engine when the cloud LLM is unavailable.
7. Rule-based thresholds provide a final safety fallback so the system never depends entirely on LLM output.

## Safety and Security Design

- The ESP32-S3 only collects data and executes commands; it never calls the LLM API directly.
- The LLM API key is stored only in cloud environment variables.
- IoTDA property reports use the official topic and JSON structure.
- IoTDA command delivery uses the official command format.
- The cloud service validates every LLM result before converting it into a device command.
- Buzzer commands use allowlisted parameters to prevent arbitrary GPIO control.
- Local Ollama runs only on a server, edge gateway, or demonstration computer, never on the ESP32-S3.

## Recommended Presentation Wording

```text
This project presents an intelligent environment monitoring and alerting system based on the ESP32-S3, Huawei Cloud IoTDA, and a cloud LLM analysis service. The device collects temperature and humidity readings in real time through an SHT30 sensor and reports them to the cloud over MQTT/MQTTS using the official Huawei Cloud IoTDA device property format. IoTDA manages device access, product models, device shadows, data forwarding, and cloud command delivery.

After receiving data forwarded by IoTDA, the cloud analysis service maintains recent temperature and humidity trends and calls a cloud LLM API to assess environmental risk. The LLM returns a risk level, an explanation of abnormal conditions, recommended actions, and an alarm decision. The cloud service validates the structured result, applies rule-based thresholds as a safety fallback, and controls the ESP32-S3 buzzer/GPIO through the IoTDA command interface.

The system forms a complete closed loop from data collection and cloud access through intelligent analysis and platform control to local alerts. Compared with traditional threshold alarms, it can understand trends, explain risks, and generate useful recommendations. Keeping the API key and inference service in the cloud also improves security, scalability, and presentation value.
```

## Backup Reasoning Wording

```text
To improve reliability, the project uses a multi-level analysis mechanism: the cloud LLM API is the primary risk reasoning engine, local Ollama provides backup analysis when the cloud API is unavailable, and rule-based thresholds act as the final safety fallback. This design preserves basic analysis and alerting when the network fails, API quota is exhausted, or the cloud model service is unavailable.
```
