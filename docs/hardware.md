# Hardware Design

This document describes the edge device part of the project.

The ESP32-S3 is not responsible for LLM inference. It only handles sensing, display, cloud connection, command reception, and physical alert execution.

## Hardware Components

| Component | Role |
|---|---|
| ESP32-S3 | Main controller, Wi-Fi, MQTT/MQTTS, GPIO control |
| SHT30 | Temperature and humidity sensor |
| OLED | Local display for readings and alert state |
| Buzzer | Audible warning output |

## Device-Side Responsibilities

ESP32-S3 should implement:

- Read temperature and humidity from SHT30 through I2C.
- Apply simple smoothing or median filtering if needed.
- Connect to Wi-Fi.
- Connect to Huawei Cloud IoTDA through MQTT/MQTTS.
- Report sensor readings as IoTDA device properties.
- Subscribe to IoTDA command topics.
- Parse downlink commands.
- Control buzzer/GPIO according to the command.
- Display readings, connection state, and alert state on OLED.
- Return command execution result to IoTDA.

## Recommended I2C Wiring

Typical wiring:

```text
ESP32-S3        SHT30          OLED
3.3V      ->    VIN       ->    VCC
GND       ->    GND       ->    GND
SDA       ->    SDA       ->    SDA
SCL       ->    SCL       ->    SCL
```

Use the actual SDA and SCL pins defined in your firmware and board configuration.

## Buzzer Control

The buzzer should be controlled through a GPIO pin, for example:

```text
BUZZER_PIN -> buzzer signal input
GND        -> buzzer ground
```

For competition safety and clarity, the buzzer should only support a small set of actions:

| Action | Meaning |
|---|---|
| off | Stop alert |
| on | Start alert |
| slow_beep | Low or medium risk |
| fast_beep | High risk |
| continuous | Critical risk |

The exact mode can be mapped from the IoTDA command parameter in firmware.

## Local Safety Fallback

Even when cloud reasoning is unavailable, the firmware may keep a minimal local safety rule:

```text
temperature >= 45 C -> local buzzer alert
humidity >= 95 %RH  -> local buzzer alert
```

This is not the main intelligence of the system. It is only a final safety fallback.

## What the Device Must Not Do

- Do not store cloud LLM API keys in firmware.
- Do not call DeepSeek, Tongyi, Pangu, OpenAI, or other LLM APIs directly from ESP32-S3.
- Do not run Ollama on ESP32-S3.
- Do not expose GPIO control through a public network service.
- Do not invent a private IoTDA property report format.

## Firmware Configuration Notes

The local `firmware/src/config.h` file should contain only device-local settings and should not be committed.

Use `firmware/src/config.example.h` as a template. Real values such as Wi-Fi password, IoTDA device secret, and server credentials should stay local.
