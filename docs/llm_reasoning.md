# LLM Reasoning

This document defines how LLM reasoning fits into the cloud-based intelligent environment monitoring system.

The LLM is not a chatbot in this project. It is a cloud-side risk analysis module.

## Position in the System

```text
IoTDA forwarded sensor data
  -> cloud analysis service
  -> primary cloud LLM API
  -> local Ollama backup if needed
  -> rule fallback if needed
  -> validated risk decision
  -> IoTDA command downlink
```

ESP32-S3 does not run LLM inference. It only receives the final IoTDA command and controls buzzer/GPIO.

## Reasoning Priority

```text
1. Cloud LLM API
2. Local Ollama backup
3. Rule threshold fallback
```

Primary cloud LLM options:

- DeepSeek
- Tongyi
- Pangu
- OpenAI

Local backup:

- Ollama running on a local server, edge gateway, or competition demo laptop

The backup design is useful for demo reliability, but Ollama should not become an ESP32-S3 responsibility.

## What the LLM Should Analyze

The LLM should analyze:

- Recent temperature trend.
- Recent humidity trend.
- Whether values are abnormal.
- Whether data shows continuous rise, continuous drop, or sudden fluctuation.
- Possible environmental causes.
- Risk level.
- Suggested handling.
- Whether a buzzer alert is recommended.

The LLM should not:

- Talk to the user as a chatbot.
- Generate arbitrary device commands.
- Directly control GPIO.
- Replace deterministic safety rules.

## Internal Input to LLM

The cloud service may send the recent sensor window to the LLM in an internal format like this:

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
  },
  {
    "time": "10:00:20",
    "temperature": 29.1,
    "humidity": 78.6
  },
  {
    "time": "10:00:30",
    "temperature": 30.2,
    "humidity": 84.1
  }
]
```

This is not the ESP32-S3 to IoTDA property report format. It is only an internal cloud service to LLM payload.

## Prompt Template

```text
You are the cloud risk analysis module for an IoT environment monitoring system.

Your task is not to chat. Assess environmental risk from the recent temperature and humidity readings.

Follow these rules:
1. Determine whether the temperature or humidity is abnormal.
2. Detect sustained increases, sustained decreases, or sharp fluctuations.
3. Explain likely causes in the context of environment monitoring.
4. Return one risk level: normal, low, medium, high, or critical.
5. Provide one or two recommended actions.
6. Decide whether the buzzer alarm should be triggered.
7. Return JSON only, without Markdown or additional explanation.
8. Write every human-readable text field in concise English using ASCII characters only. Do not output Chinese or other non-ASCII characters.

Risk guidelines:
- Temperature > 35 C: high-temperature risk
- Humidity > 80 %RH: high-humidity risk
- Sustained humidity increase: possible condensation, water leakage, or poor ventilation
- Sharp short-term temperature or humidity changes: possible sensor fault or sudden environmental change
- Temperature > 40 C or humidity > 90 %RH: critical risk

Recent environment readings:
{{recent_sensor_data}}

Return JSON in the following format:
{
  "risk_level": "normal | low | medium | high | critical",
  "risk_score": 0,
  "abnormal_reason": "",
  "trend_analysis": "",
  "suggestions": [],
  "alarm_required": false,
  "buzzer_value": "0 | 1",
  "buzzer_pattern": "none | slow_beep | fast_beep | continuous"
}
```

## Expected LLM Output

```json
{
  "risk_level": "high",
  "risk_score": 82,
  "abnormal_reason": "Humidity is rising above 80 %RH, creating moisture and condensation risk.",
  "trend_analysis": "Humidity rose from 68.2 %RH to 84.1 %RH in 30 seconds.",
  "suggestions": [
    "Check for moisture or water leaks near the device.",
    "Improve ventilation and keep the sensor away from damp areas."
  ],
  "alarm_required": true,
  "buzzer_value": "1",
  "buzzer_pattern": "fast_beep"
}
```

## Output Validation

The cloud analysis service must validate the LLM result before sending any command.

Required checks:

| Field | Rule |
|---|---|
| `risk_level` | Must be `normal`, `low`, `medium`, `high`, or `critical` |
| `risk_score` | Must be 0 to 100 |
| `alarm_required` | Must be boolean |
| `buzzer_value` | Must be `0` or `1` |
| `buzzer_pattern` | Must be allowlisted |
| `suggestions` | Should be short and display-safe |

Invalid LLM output should not control the device. Use rule fallback instead.

## Convert Reasoning Result to IoTDA Command

LLM output is internal to the cloud service. The final downlink to ESP32-S3 must use Huawei Cloud IoTDA command format.

Example final command:

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

The device receives this command from:

```text
$oc/devices/{device_id}/sys/commands/#
```

Then it responds through:

```text
$oc/devices/{device_id}/sys/commands/response/request_id={request_id}
```

## Local Ollama Backup

Ollama can be kept as a backup reasoning engine.

Recommended positioning:

```text
Cloud LLM unavailable
  -> cloud analysis service calls local Ollama
  -> if Ollama succeeds, validate output
  -> convert to IoTDA command
```

Ollama may run on:

- Local server
- Edge gateway
- Competition demo laptop

Ollama should not run on:

- ESP32-S3
- Sensor firmware

Recommended competition wording:

```text
系统默认使用云端大模型 API 进行智能环境风险分析。当云端 LLM 服务不可用、网络异常或 API 调用失败时，系统可切换到本地 Ollama 模型作为备用分析引擎，保证系统仍具备基础智能分析和告警决策能力。Ollama 运行在本地服务器或边缘网关，不运行在 ESP32-S3 设备端。
```

## Security Boundaries

- LLM API keys must stay in cloud environment variables.
- ESP32-S3 must not store LLM API keys.
- ESP32-S3 must not call cloud LLM APIs directly.
- ESP32-S3 must not run Ollama.
- IoTDA property reports must use the official `services` structure.
- IoTDA command downlink must use the official command structure.
- LLM output must be validated before command generation.
- Real secrets must not be printed in logs.

## Related IoTDA Formats

Device property report topic:

```text
$oc/devices/{device_id}/sys/properties/report
```

Device property report body:

```json
{
  "services": [
    {
      "service_id": "Environment",
      "properties": {
        "temperature": 28.6,
        "humidity": 72.3
      }
    }
  ]
}
```

Command downlink body:

```json
{
  "command_name": "BuzzerControl",
  "service_id": "Alarm",
  "paras": {
    "value": "1"
  }
}
```

Official references:

- Huawei Cloud IoTDA device property report: https://support.huaweicloud.com/api-iothub/iot_06_v5_3010.html
- Huawei Cloud IoTDA platform command downlink: https://support.huaweicloud.com/api-iothub/iot_06_v5_3014.html
