# Edge IoT Monitor

Edge IoT Monitor is a competition-oriented intelligent environment monitoring and alerting system. It uses an ESP32-S3 with an SHT30 temperature and humidity sensor, OLED display, and buzzer, then connects the device to Huawei Cloud IoTDA for cloud-based data ingestion, LLM-assisted risk reasoning, and command-based alert control.

The project direction is no longer a local chatbot on embedded hardware. The ESP32-S3 focuses on sensing and execution, while cloud services handle device access, data forwarding, LLM reasoning, and downlink commands.

```text
ESP32-S3 + SHT30
  -> Huawei Cloud IoTDA
  -> Cloud analysis service
  -> Cloud LLM API, with local Ollama as backup
  -> IoTDA command downlink
  -> ESP32-S3 buzzer / OLED alert
```

## Project Goal

Build a closed-loop cloud intelligent monitoring system:

- Collect temperature and humidity from SHT30.
- Report device properties to Huawei Cloud IoTDA using the official device-side MQTT format.
- Forward IoTDA data to a cloud analysis service.
- Use a cloud LLM to analyze recent environmental trends and risk.
- Keep local Ollama as a backup reasoning engine, not as the main device-side design.
- Downlink alert commands through IoTDA.
- Let ESP32-S3 control buzzer/GPIO and display state on OLED.

## Hardware

| Component | Role |
|---|---|
| ESP32-S3 | Wi-Fi device, sensor acquisition, IoTDA connection, command execution |
| SHT30 | Temperature and humidity sensing |
| OLED | Local reading, network, and alert status display |
| Buzzer | Audible alert output controlled by GPIO |

See [hardware.md](docs/hardware.md) for wiring and device-side responsibilities.

## Architecture

The recommended competition architecture is:

```text
ESP32-S3
  -> MQTT/MQTTS property report
  -> Huawei Cloud IoTDA
  -> data forwarding
  -> FunctionGraph / ECS / container service
  -> LLM risk reasoning
  -> IoTDA command downlink
  -> buzzer alert
```

Detailed documents:

- [architecture.md](docs/architecture.md): system overview
- [cloud_iotda.md](docs/cloud_iotda.md): Huawei Cloud IoTDA topics, product model, property report, and command downlink
- [llm_reasoning.md](docs/llm_reasoning.md): LLM prompt, risk reasoning, cloud API, and Ollama backup
- [backend_service.md](docs/backend_service.md): cloud analysis service responsibilities
- [data_flow.md](docs/data_flow.md): end-to-end data and command flow
- [competition_writeup.md](docs/competition_writeup.md): competition-ready wording

## Current Repository Status

The repository still contains a working local prototype:

| Area | Current state |
|---|---|
| Firmware | PlatformIO + Arduino prototype for Wi-Fi, SHT30, OLED, HTTP ingest, and command polling |
| Backend | C++ HTTP service with ingest, SQLite storage, anomaly detection, and local command queue |
| Local LLM | Optional Ollama analysis for local backup reasoning |
| Cloud integration | Being redesigned around Huawei Cloud IoTDA and cloud LLM reasoning |

The existing local backend is useful as a fallback and development harness. The competition architecture should present IoTDA as the primary device access and command channel.

## Local Prototype Quick Start

Create local backend config:

```bash
cp config/config.example.yaml config/config.yaml
```

Build and run the backend:

```bash
cmake -S . -B build-cmake -DEDGE_BUILD_SERVER=ON
cmake --build build-cmake
./build-cmake/backend/edge_server
```

Send simulated readings:

```bash
python3 scripts/simulate_sensor.py --device esp32s3-001 --count 10
```

Run backend smoke test:

```bash
python3 scripts/test_backend_api.py
```

## Firmware Setup

Install PlatformIO if needed:

```bash
python3 -m pip install --user -r requirements-dev.txt
```

Build firmware:

```bash
cd firmware
pio run
```

Create local firmware config before flashing:

```bash
cp src/config.example.h src/config.h
```

`src/config.h` is local and sensitive. Do not commit Wi-Fi credentials, IoTDA secrets, or API keys.

## Security Boundaries

- Do not put LLM API keys in ESP32 firmware.
- Do not let ESP32-S3 call DeepSeek, Tongyi, Pangu, or OpenAI directly.
- Do not run Ollama on ESP32-S3.
- Do not invent custom IoTDA outer JSON formats.
- Do not expose device GPIO control directly to the public network.
- Validate LLM output before converting it into IoTDA commands.
- Keep real credentials in local config or cloud environment variables only.

## License

MIT
