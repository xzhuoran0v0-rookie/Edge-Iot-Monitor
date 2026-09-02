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
#define IOTDA_EDGE_SERVICE_ID "EdgeReasoning"

// ---------------- Buzzer ----------------
// Set to 1 only after the module and its wiring pass a hardware test. While 0
// the GPIO is held high-impedance so no command can energise an unverified
// circuit. On boot the firmware emits a short self-test beep, which is the
// fastest way to confirm both the wiring and the active level below.
#define ENABLE_BUZZER 0

// Which logic level makes YOUR module sound. Most active-buzzer breakouts are
// active-low, but plenty are active-high, and guessing wrong means the buzzer
// screams continuously from power-on and cannot be silenced.
//
// To find out in 30 seconds, with VCC and GND connected: touch the module's
// I/O pin to GND — if it sounds, it is LOW. Touch it to 3V3 — if it sounds
// there instead, change this to HIGH.
#define BUZZER_ACTIVE_LEVEL LOW

// Local alarm thresholds. Crossing either sounds the buzzer immediately, on
// the device, with no network involved — this is the one alarm path that
// cannot be broken by Wi-Fi, the cloud, or an API quota.
//
// These are deliberately lower than the AdaptiveBaseline hard limits (45 C /
// 95 %RH) so the alarm is reachable without special equipment.
//
// Cupping the sensor in a hand trips HUMIDITY first, not temperature: palm skin
// is near saturation and fills the enclosed air within seconds, while heat has
// to conduct through the housing. To raise temperature without moisture, hold a
// warm object 1-2 cm away.
#define ALARM_TEMP_C 30.0f
#define ALARM_HUMIDITY_PCT 70.0f
#define IOTDA_ALARM_SERVICE_ID "Alarm"
#define IOTDA_BUZZER_COMMAND_NAME "BuzzerControl"

// ---------------- Timing ----------------
// Two separate intervals. One value for both would force a choice between a
// responsive display and a small cloud message budget.
//
// SENSE_INTERVAL_MS drives reasoning, the OLED, and the local backend — all
// free. Shorten it to make the screen react faster.
//
// CLOUD_INTERVAL_MS drives the IoTDA publish only, which is metered. Against a
// 10 000 msg/day free tier:
//   10 s -> 8 640/day (86%, one device fills the quota)
//   30 s -> 2 880/day (29%)
//   60 s -> 1 440/day (14%, roughly six devices share one quota)
// Lengthening it costs nothing locally, and is what makes more than one device
// fit in a free allowance.
//
// The local alarm is unaffected by both: it runs in the 1 Hz safety task and
// responds within a second regardless of what is set here.
#define SENSE_INTERVAL_MS 2000
#define CLOUD_INTERVAL_MS 60000

// TLS note:
// MQTTS 8883 validates the IoTDA server certificate with the CA in src/certs.h.
