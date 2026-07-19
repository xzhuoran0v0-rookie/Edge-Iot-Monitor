# Cloud Analysis Service

The cloud analysis service is the bridge between Huawei Cloud IoTDA and the reasoning engines.

It can be deployed on:

- FunctionGraph
- ECS
- Cloud container service
- A competition demo server

## Core Responsibilities

The service should:

1. Receive data forwarded by IoTDA.
2. Parse IoTDA `services` data.
3. Extract `temperature` and `humidity` from the `Environment` service.
4. Keep a recent sensor data window.
5. Build an LLM prompt for trend and risk analysis.
6. Call the primary cloud LLM API.
7. Fall back to local Ollama if the cloud LLM fails.
8. Fall back to deterministic rules if all LLM calls fail.
9. Validate the reasoning result.
10. Convert the final decision into an IoTDA command.
11. Call IoTDA command downlink API.
12. Record logs for demo and debugging.

## Reasoning Priority

```text
1. Cloud LLM API
2. Local Ollama backup
3. Rule threshold fallback
```

Recommended cloud LLM options:

- DeepSeek
- Tongyi
- Pangu
- OpenAI

Recommended local backup:

- Ollama running on a local server, demo laptop, or edge gateway

## Environment Variables

Cloud secrets should be stored in environment variables, not in firmware.

Example:

```text
LLM_PROVIDER=deepseek
LLM_API_KEY=your_cloud_llm_api_key
LLM_BASE_URL=https://api.example.com
OLLAMA_BASE_URL=http://127.0.0.1:11434
HUAWEI_IOTDA_PROJECT_ID=your_project_id
HUAWEI_IOTDA_ACCESS_KEY=your_access_key
HUAWEI_IOTDA_SECRET_KEY=your_secret_key
```

Never print full keys in logs.

## Recent Data Window

The service should keep recent readings per device.

Recommended demo settings:

| Item | Value |
|---|---|
| Window length | Last 10 to 30 readings |
| Normal upload interval | 5 to 30 seconds |
| Competition demo interval | 3 to 5 seconds |
| Analysis trigger | Every new reading or every N readings |

Example internal window:

```json
[
  {
    "time": "10:00:00",
    "temperature": 27.8,
    "humidity": 68.2
  },
  {
    "time": "10:00:10",
    "temperature": 28.4,
    "humidity": 72.5
  }
]
```

This internal JSON is not the IoTDA device property report format. It is only used inside the cloud analysis service and LLM prompt.

## Validation Before Command Downlink

LLM output must be validated before controlling hardware.

Check:

- `risk_level` is one of `normal`, `low`, `medium`, `high`, `critical`.
- `risk_score` is between 0 and 100.
- `alarm_required` is boolean.
- `buzzer_value` is `0` or `1`.
- `buzzer_pattern` is allowlisted.
- Command duration does not exceed the configured maximum.

If validation fails, ignore the LLM command fields and use rule fallback.

## Rule Fallback

Recommended fallback rules:

```text
temperature > 40 C -> high risk, buzzer on
temperature > 45 C -> critical risk, buzzer on
humidity > 85 %RH  -> high risk, buzzer on
humidity > 95 %RH  -> critical risk, buzzer on
rapid humidity rise -> medium or high risk
```

The exact thresholds can be tuned for the competition scenario.

## Command Generation

The service should convert the final decision into the IoTDA command format.

Example:

```json
{
  "command_name": "BuzzerControl",
  "service_id": "Alarm",
  "paras": {
    "value": "1",
    "duration_ms": 3000,
    "pattern": "fast_beep"
  }
}
```

The ESP32-S3 receives the command through IoTDA and executes it locally.
