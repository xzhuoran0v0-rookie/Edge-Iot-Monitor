#include "wifi_manager.h"

// 静态成员定义
const char *WiFiManager::ssid_ = nullptr;
const char *WiFiManager::password_ = nullptr;

bool WiFiManager::init(const char *ssid,
                       const char *password,
                       uint32_t timeout_ms)
{
    ssid_ = ssid;
    password_ = password;

    Serial.print("[WiFi] Connecting to ");
    Serial.println(ssid_);

    // 断开之前可能存在的连接
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid_, password_);

    uint32_t start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - start > timeout_ms)
        {
            Serial.println("[WiFi] Connection timeout!");
            return false;
        }
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());

    return true;
}

bool WiFiManager::isConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

void WiFiManager::reconnectIfNeeded()
{
    if (isConnected())
        return;

    Serial.println("[WiFi] Lost connection, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(ssid_, password_);

    // 非阻塞：只尝试一次，下次 loop() 再检查
    
}

String WiFiManager::localIP()
{
    return WiFi.localIP().toString();
}