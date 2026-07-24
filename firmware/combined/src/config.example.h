#pragma once

// Copy this file to src/config.h and fill in real values.
// src/config.h is ignored by Git.

// ---------------- WiFi ----------------
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASS "your_wifi_password"

// ---------------- Local backend path ----------------
#define SERVER_URL "http://192.168.x.x:8080/api/ingest"
#define DEVICE_ID  "esp32s3-001"
#define BUZZER_PIN 4

// ---------------- Huawei Cloud IoTDA (MQTTS) ----------------
#define IOTDA_MQTT_HOST "your-iotda-mqtt-host"
#define IOTDA_MQTT_PORT 8883
#define IOTDA_DEVICE_ID "your_iotda_device_id"
#define IOTDA_DEVICE_SECRET "your_iotda_device_secret"

// Pre-computed MQTT auth strings (client_id / username / password) derived from
// device_id, secret and the UTC-hour timestamp per IoTDA docs.
#define IOTDA_MQTT_CLIENT_ID "your_iotda_mqtt_client_id"
#define IOTDA_MQTT_USERNAME  "your_iotda_mqtt_username"
#define IOTDA_MQTT_PASSWORD  "your_iotda_mqtt_password"

// Product model names.
#define IOTDA_SERVICE_ID "Environment"
#define IOTDA_ALARM_SERVICE_ID "Alarm"
#define IOTDA_BUZZER_COMMAND_NAME "BuzzerControl"

// Report interval for both the local and cloud paths.
#define REPORT_INTERVAL_MS 10000

// TLS note:
// MQTTS 8883 validates the IoTDA server certificate with the CA in src/certs.h.
