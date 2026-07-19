#pragma once

#define WIFI_SSID    "your_wifi_name"
#define WIFI_PASS    "your_password"

// Local backup backend. Keep this for offline demo and fallback testing.
#define SERVER_URL   "http://192.168.x.x:8080/api/ingest"

// Device identity used by both the local backup path and Huawei Cloud IoTDA.
#define DEVICE_ID    "esp32s3-001"
#define BUZZER_PIN   4

// Huawei Cloud IoTDA placeholders. Fill these after creating the product
// and device in IoTDA. Do not commit real secrets in src/config.h.
#define IOTDA_ENABLED        0
#define IOTDA_MQTT_HOST     "your-iotda-mqtt-host"
#define IOTDA_MQTT_PORT     8883
#define IOTDA_DEVICE_ID     "your_iotda_device_id"
#define IOTDA_DEVICE_SECRET "your_iotda_device_secret"
#define IOTDA_SERVICE_ID    "Environment"
#define IOTDA_ALARM_SERVICE "Alarm"
#define IOTDA_BUZZER_CMD    "BuzzerControl"
#define REPORT_INTERVAL_MS  10000

// Huawei Cloud IoTDA MQTT auth parameters.
// Generate these from device_id, secret, and timestamp according to IoTDA docs.
// Keep real values only in src/config.h.
#define IOTDA_MQTT_CLIENT_ID "your_iotda_mqtt_client_id"
#define IOTDA_MQTT_USERNAME  "your_iotda_mqtt_username"
#define IOTDA_MQTT_PASSWORD  "your_iotda_mqtt_password"

// TLS note:
// MQTTS 8883 should use WiFiClientSecure with the Huawei Cloud IoTDA server
// certificate chain trusted by a root CA. Do not use setInsecure() except
// for short-lived local debugging.
