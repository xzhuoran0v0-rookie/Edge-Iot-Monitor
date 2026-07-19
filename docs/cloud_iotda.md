# Huawei Cloud IoTDA Design

This document records the Huawei Cloud IoTDA side of the project.

The key rule is: device-side messages must follow Huawei Cloud IoTDA official MQTT/MQTTS topics and JSON structure. The project may define product model services and properties, but it should not invent a private outer protocol.

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

IoTDA should forward reported device data to the cloud analysis service through a rule or integration path. The cloud service then:

1. Parses `services`.
2. Finds `service_id = Environment`.
3. Reads `temperature` and `humidity`.
4. Appends the values to the recent trend window.
5. Runs LLM reasoning and rule fallback.
6. Calls IoTDA command downlink if alert is needed.

## Official References

- Huawei Cloud IoTDA device property report: https://support.huaweicloud.com/api-iothub/iot_06_v5_3010.html
- Huawei Cloud IoTDA platform command downlink: https://support.huaweicloud.com/api-iothub/iot_06_v5_3014.html
