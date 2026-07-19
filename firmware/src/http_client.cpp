#include "http_client.h"
#include "config.h"
#include <ArduinoJson.h>

#ifndef DEVICE_ID
#define DEVICE_ID "esp32s3-001"
#endif

#ifndef BUZZER_PIN
#define BUZZER_PIN 4
#endif

void HttpClient::initActuators()
{
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
}

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
    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = millis() / 1000;
    doc["temperature"] = roundf(temp * 10.0f) / 10.0f;
    doc["humidity"] = roundf(humi * 10.0f) / 10.0f;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

void HttpClient::pollAndApplyCommand()
{
    if (!WiFi.isConnected())
        return;

    HTTPClient http;
    String url = commandBaseUrl() + "/api/commands/next?device_id=" + String(DEVICE_ID);

    http.begin(url);
    int code = http.GET();
    if (code != 200)
    {
        Serial.print("[CMD] Poll failed: ");
        Serial.println(code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        Serial.print("[CMD] JSON parse failed: ");
        Serial.println(err.c_str());
        return;
    }

    const char *status = doc["status"] | "";
    if (String(status) == "idle")
        return;
    if (String(status) != "ok")
    {
        Serial.print("[CMD] Unexpected response: ");
        Serial.println(body);
        return;
    }

    if (!doc["command_id"].is<int>() || !doc["command"].is<const char *>() || !doc["duration_ms"].is<int>())
    {
        Serial.print("[CMD] Bad command payload: ");
        Serial.println(body);
        return;
    }

    int commandId = doc["command_id"];
    int durationMs = doc["duration_ms"];
    String command = doc["command"].as<const char *>();

    bool ok = applyCommand(command, durationMs);
    ackCommand(commandId, ok ? "done" : "failed");
}

String HttpClient::commandBaseUrl()
{
    String url = String(SERVER_URL);
    int pos = url.indexOf("/api/ingest");
    if (pos >= 0)
        return url.substring(0, pos);
    return url;
}

void HttpClient::ackCommand(int commandId, const String &result)
{
    HTTPClient http;
    http.begin(commandBaseUrl() + "/api/commands/ack");
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
    doc["command_id"] = commandId;
    doc["result"] = result;

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    Serial.print("[CMD] ACK code: ");
    Serial.println(code);
    http.end();
}

bool HttpClient::applyCommand(const String &command, int durationMs)
{
    if (command == "buzzer_on")
    {
        if (durationMs < 0 || durationMs > 30000)
            return false;

        Serial.print("[CMD] buzzer_on ");
        Serial.print(durationMs);
        Serial.println("ms");
        digitalWrite(BUZZER_PIN, HIGH);
        if (durationMs > 0)
        {
            delay(durationMs);
            digitalWrite(BUZZER_PIN, LOW);
        }
        return true;
    }

    if (command == "buzzer_off")
    {
        Serial.println("[CMD] buzzer_off");
        digitalWrite(BUZZER_PIN, LOW);
        return true;
    }

    Serial.print("[CMD] Rejected command: ");
    Serial.println(command);
    return false;
}
