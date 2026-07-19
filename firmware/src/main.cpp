#include <Arduino.h>
#include "config.h"
#include "wifi_manager.h"
#include "http_client.h"
#include "iotda_client.h"
#include "sht30.h"
#include "oled.h"
#include "median_filter.h"

void setup()
{
    Serial.begin(115200);
    delay(3000);

    Serial.println("=== Edge IoT Monitor ===");
    HttpClient::initActuators();
    IotdaClient::logPlannedTopics();

    // OLED 初始化
    OLED::init();
    OLED::showStatus("STARTING...");

    // WiFi 连接
    OLED::showStatus("WIFI CONNECTING");
    if (!WiFiManager::init(WIFI_SSID, WIFI_PASS, 20000))
    {
        Serial.println("[ERROR] WiFi failed");
        OLED::showStatus("WIFI FAILED");
        while (true) delay(1000);
    }
    Serial.println("[OK] WiFi connected");
    OLED::showStatus("WIFI OK");

    // SHT30 初始化
    if (!SHT30::init())
    {
        Serial.println("[ERROR] SHT30 failed");
        OLED::showStatus("SHT30 FAILED");
        while (true) delay(1000);
    }
    Serial.println("[OK] SHT30 ready");
}

void loop()
{
    WiFiManager::reconnectIfNeeded();

    float temp = 0, humi = 0;

    if (!SHT30::read(temp, humi))
    {
        Serial.println("[WARN] SHT30 read failed");
        delay(5000);
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
