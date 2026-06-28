#pragma once

// Copy this file to src/config.h and fill in real values.
// src/config.h is ignored by Git.

#define WIFI_SSID "your_wifi_name"
#define WIFI_PASS "your_wifi_password"

// Huawei Cloud IoTDA MQTT access.
// Fill these after creating the IoTDA product and device.
#define IOTDA_MQTT_HOST "your-iotda-mqtt-host"
#define IOTDA_MQTT_PORT 8883
#define IOTDA_DEVICE_ID "your_iotda_device_id"
#define IOTDA_DEVICE_SECRET "your_iotda_device_secret"

// Product model names used by this project.
#define IOTDA_SERVICE_ID "Environment"
#define IOTDA_ALARM_SERVICE_ID "Alarm"
#define IOTDA_BUZZER_COMMAND_NAME "BuzzerControl"

// MVP report interval.
#define REPORT_INTERVAL_MS 10000

// TLS note:
// The MVP validates the IoTDA server certificate with the CA in src/certs.h.
