#include <Arduino.h>
#include "config.h"
#include "wifi_manager.h"
#include "http_client.h"
#include "iotda_client.h"
#include "sht30.h"
#include "oled.h"
#include "median_filter.h"

namespace
{
constexpr unsigned long SENSOR_RETRY_INTERVAL_MS = 2000;

bool sensor_ready = false;
unsigned long last_sensor_retry_ms = 0;

bool ensureSensorReady(bool force = false)
{
    if (sensor_ready)
        return true;

    const unsigned long now = millis();
    if (!force && now - last_sensor_retry_ms < SENSOR_RETRY_INTERVAL_MS)
        return false;

    last_sensor_retry_ms = now;
    Serial.println("[SHT30] Trying to connect...");

    if (!SHT30::init())
    {
        Serial.println("[SHT30] Offline; uploads remain blocked");
        OLED::showStatus("SHT30 OFFLINE");
        return false;
    }

    sensor_ready = true;
    Serial.println("[SHT30] Reconnected");
    OLED::showStatus("SHT30 ONLINE");
    delay(300);
    return true;
}

void markSensorOffline()
{
    sensor_ready = false;
    last_sensor_retry_ms = millis();
    Serial.println("[SHT30] Lost connection; uploads blocked");
    OLED::showStatus("SHT30 OFFLINE");
}
} // namespace

void setup()
{
    Serial.begin(115200);
    delay(3000);

    Serial.println("=== Edge IoT Monitor ===");
    HttpClient::initActuators();
    HttpClient::testBuzzer();
    IotdaClient::logPlannedTopics();

    // OLED 初始化
    OLED::init();
    OLED::showStatus("STARTING...");

    // Probe the sensor before waiting for the network. Sensor health must not
    // depend on Wi-Fi availability.
    ensureSensorReady(true);

    // WiFi 连接
    OLED::showStatus("WIFI CONNECTING");
    if (!WiFiManager::init(WIFI_SSID, WIFI_PASS, 20000))
    {
        Serial.println("[WiFi] Offline; network uploads unavailable");
        OLED::showStatus(sensor_ready ? "WIFI OFFLINE" : "SHT30 OFFLINE");
    }
    else
    {
        Serial.println("[OK] WiFi connected");
        OLED::showStatus(sensor_ready ? "SHT30 ONLINE" : "SHT30 OFFLINE");
    }
}

void loop()
{
    WiFiManager::reconnectIfNeeded();

    // 传感器离线时阻止数据上报，但保留自动恢复能力。
    if (!ensureSensorReady())
    {
        delay(50);
        return;
    }

    float temp = 0, humi = 0;

    if (!SHT30::read(temp, humi))
    {
        markSensorOffline();
        return;
    }

    temp=medianFilterTemp(temp);
    humi=medianFilterHumi(humi);

    Serial.print("[SENSOR] Temp=");
    Serial.print(temp, 1);
    Serial.print("C  Humi=");
    Serial.print(humi, 1);
    Serial.println("%");

    // 显示到 OLED
    OLED::showSensorData(temp, humi);

    // Local backup path: send to the local backend while IoTDA credentials
    // are not configured yet.
    HttpClient::postSensorData(temp, humi);
    HttpClient::pollAndApplyCommand();

    // IoTDA preparation: build the official property report payload now.
    // Actual MQTT/MQTTS publishing will be enabled after IoTDA registration.
#if IOTDA_ENABLED
    Serial.print("[IoTDA] Prepared report: ");
    Serial.println(IotdaClient::buildPropertyReport(temp, humi));
#endif

    delay(5000);
}
