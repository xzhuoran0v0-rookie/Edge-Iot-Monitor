# Huawei Cloud IoTDA Design

This document records the Huawei Cloud IoTDA side of the project: topics,
product model, property report, and command downlink.

The key rule is: device-side messages must follow Huawei Cloud IoTDA official MQTT/MQTTS topics and JSON structure. The project may define product model services and properties, but it should not invent a private outer protocol.

## What IoTDA is for here

IoTDA is the **transport and device-access layer**. It carries conclusions the
ESP32-S3 has already reached; it is not part of the decision path. See
[edge_reasoning.md](edge_reasoning.md) for where decisions are actually made.

The device publishes two services in one combined message per 10-second cycle:

| `service_id` | Properties |
|---|---|
| `Environment` | `temperature`, `humidity` |
| `EdgeReasoning` | `state`, `severity`, `confidence`, `reason_code` |

`EdgeReasoning` is the device's own verdict. Anything consuming it should
display or record it — not recompute it.

## Implementation status

| Direction | Status |
|---|---|
| Device → IoTDA property report (MQTT/MQTTS) | **Implemented** in `firmware/combined`; needs registered device credentials |
| IoTDA → device command downlink | Topics and format implemented in firmware |
| Backend → IoTDA data forwarding / command API | **Not implemented** — `CloudSync` is a skeleton |

The "Data Forwarding" section below therefore describes an integration design,
not shipped behaviour.

## Device Property Report

Official topic:

```text
$oc/devices/{device_id}/sys/properties/report
```

Optional `request_id`:

```text
$oc/devices/{device_id}/sys/properties/report?request_id={request_id}
```

Official message structure:

```json
{
  "services": [
    {
      "service_id": "Temperature",
      "properties": {
        "value": 57,
        "value2": 60
      },
      "event_time": "20151212T121212Z"
    }
  ]
}
```

Field meanings:

| Field | Required | Meaning |
|---|---|---|
| `services` | yes | Device service data list |
| `service_id` | yes | Service ID defined in the IoTDA product model |
| `properties` | yes | Service properties defined in the IoTDA product model |
| `event_time` | no | Device-side UTC collection time |

## Recommended Product Model

Service ID:

```text
Environment
```

Properties:

| Property | Type | Meaning |
|---|---|---|
| `temperature` | decimal / number | SHT30 temperature in Celsius |
| `humidity` | decimal / number | SHT30 relative humidity in %RH |

Project property report example:

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

If reliable UTC time is available:

```json
{
  "services": [
    {
      "service_id": "Environment",
      "properties": {
        "temperature": 28.6,
        "humidity": 72.3
      },
      "event_time": "2026-06-25T10:30:00.000Z"
    }
  ]
}
```

## Platform Command Downlink

Platform command topic:

```text
$oc/devices/{device_id}/sys/commands/request_id={request_id}
```

Device response topic:

```text
$oc/devices/{device_id}/sys/commands/response/request_id={request_id}
```

Device subscription topic:

```text
$oc/devices/{device_id}/sys/commands/#
```

Official command structure:

```json
{
  "object_device_id": "{object_device_id}",
  "command_name": "ON_OFF",
  "service_id": "WaterMeter",
  "paras": {
    "value": "1"
  }
}
```

Device response structure:

```json
{
  "result_code": 0,
  "response_name": "COMMAND_RESPONSE",
  "paras": {
    "result": "success"
  }
}
```

## Recommended Alert Command Model

Service ID:

```text
Alarm
```

Command name:

```text
BuzzerControl
```

Minimal command parameter:

| Parameter | Type | Meaning |
|---|---|---|
| `value` | string / integer | `1` means buzzer on, `0` means buzzer off |

Turn buzzer on:

```json
{
  "command_name": "BuzzerControl",
  "service_id": "Alarm",
  "paras": {
    "value": "1"
  }
}
```

Turn buzzer off:

```json
{
  "command_name": "BuzzerControl",
  "service_id": "Alarm",
  "paras": {
    "value": "0"
  }
}
```

Optional richer command for demo:

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

The richer version still follows the official IoTDA command structure because custom business values stay inside `paras`.

## Data Forwarding

Not implemented — this is the design for a future `CloudSync` send path.

IoTDA would forward reported device data to a cloud service through a rule or
integration path. That service would:

1. Parse `services`.
2. Read `temperature` and `humidity` from `service_id = Environment`.
3. Read the device's verdict from `service_id = EdgeReasoning`.
4. Store both, and optionally narrate a state change for a human reader.

It would **not** re-derive the verdict. The device has already decided, using
sensor history and a learned baseline the cloud does not have, and an
independent cloud judgement that disagreed would leave the OLED and the
dashboard contradicting each other.

Any command downlink built on top of this must stay inside the existing
allowlist (`buzzer_on`, `buzzer_off`, `oled:<text>`, bounded duration).

## Official References

- Huawei Cloud IoTDA device property report: https://support.huaweicloud.com/api-iothub/iot_06_v5_3010.html
- Huawei Cloud IoTDA platform command downlink: https://support.huaweicloud.com/api-iothub/iot_06_v5_3014.html
