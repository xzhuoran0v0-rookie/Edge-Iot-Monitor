#pragma once

// Copy this file to config.h and fill in your values.
// config.h contains real credentials and must never be committed.
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASS "your_password"

#define IOTDA_MQTT_HOST "xxxxxxxxxx.st1.iotda-device.cn-north-4.myhuaweicloud.com"
#define IOTDA_MQTT_PORT 8883
#define IOTDA_DEVICE_ID "your_project_id_your-device-name"
#define IOTDA_DEVICE_SECRET "your_device_secret"

#define IOTDA_SERVICE_ID "Environment"
#define IOTDA_ALARM_SERVICE_ID "Alarm"
#define IOTDA_BUZZER_COMMAND_NAME "BuzzerControl"
#define REPORT_INTERVAL_MS 10000

// clientId format: {device_id}_0_0_{UTC timestamp yyyymmddhh}
#define IOTDA_MQTT_CLIENT_ID "your_project_id_your-device-name_0_0_2026010100"

#define IOTDA_MQTT_USERNAME "your_project_id_your-device-name"

// HMAC-SHA256(device_secret, timestamp) — see docs for how to derive
#define IOTDA_MQTT_PASSWORD "hmac_sha256_of_secret_with_timestamp"
