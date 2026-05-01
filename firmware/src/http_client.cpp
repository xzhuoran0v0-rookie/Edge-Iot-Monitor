#include "http_client.h"
#include "config.h"

bool HttpClient::postSensorData(float temp, float humi)
{
    if (!WiFi.isConnected())
    {
        Serial.println("[HTTP] WiFi not connected, skip");
        return false;
    }

    HTTPClient http;

    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    String payload = buildJson(temp, humi);

    Serial.print("[HTTP] POST ");
    Serial.println(payload);

    int code = http.POST(payload);

    Serial.print("[HTTP] Response code: ");
    Serial.println(code);

    if (code == 200)
    {
        Serial.print("[HTTP] Response: ");
        Serial.println(http.getString());
    }

    http.end();

    return code == 200;
}

String HttpClient::buildJson(float temp, float humi)
{
    // 用 millis() 当时间戳占位
    // 后端用 server_timestamp 覆盖，不影响存储
    String ts = String(millis() / 1000);

    String json = "{";
    json += "\"device_id\":\"esp32-s3-01\",";
    json += "\"timestamp\":\"" + ts + "\",";
    json += "\"temperature\":" + String(temp, 1) + ",";
    json += "\"humidity\":" + String(humi, 1);
    json += "}";

    return json;
}