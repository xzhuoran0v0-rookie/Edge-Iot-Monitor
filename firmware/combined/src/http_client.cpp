#include "http_client.h"
#include "config.h"
#include <ArduinoJson.h>

#ifndef DEVICE_ID
#define DEVICE_ID "esp32s3-001"
#endif

#ifndef BUZZER_PIN
#define BUZZER_PIN 4
#endif

#ifndef ENABLE_BUZZER
#define ENABLE_BUZZER 0
#endif

namespace
{
constexpr uint8_t BUZZER_ON_LEVEL = LOW;
constexpr uint8_t BUZZER_OFF_LEVEL = HIGH;
constexpr uint8_t MAX_BACKOFF_FAILURES = 6;
constexpr unsigned long BASE_BACKOFF_MS = 10000;
constexpr unsigned long MAX_BACKOFF_MS = 5UL * 60UL * 1000UL;
}

uint8_t HttpClient::consecutiveFailures_ = 0;
unsigned long HttpClient::backoffUntilMs_ = 0;

void HttpClient::initActuators()
{
#if ENABLE_BUZZER
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
#else
    // The current buzzer module/wiring has not passed hardware verification.
    // Keep the GPIO high-impedance so remote commands cannot energise it.
    pinMode(BUZZER_PIN, INPUT);
    Serial.println("[BUZZER] Disabled by safe firmware default");
#endif
}

bool HttpClient::backendReachable()
{
    return consecutiveFailures_ == 0;
}

void HttpClient::resetBackoff()
{
    consecutiveFailures_ = 0;
    backoffUntilMs_ = 0;
}

bool HttpClient::postSensorData(float temp, float humi,
                                const EdgeAssessment &assessment)
{
    if (!WiFi.isConnected())
        return false;

    const unsigned long now = millis();
    if (consecutiveFailures_ > 0 &&
        static_cast<int32_t>(now - backoffUntilMs_) < 0)
        return false;

    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    String payload = buildJson(temp, humi, assessment);
    int code = http.POST(payload);
    http.end();

    if (code == 200)
    {
        if (consecutiveFailures_ > 0)
        {
            Serial.println("[HTTP] Backend recovered");
            consecutiveFailures_ = 0;
        }
        backoffUntilMs_ = 0;
        return true;
    }

    if (consecutiveFailures_ < MAX_BACKOFF_FAILURES)
        ++consecutiveFailures_;
    unsigned long backoffMs = BASE_BACKOFF_MS;
    for (uint8_t i = 1; i < consecutiveFailures_; ++i)
    {
        backoffMs *= 2;
        if (backoffMs > MAX_BACKOFF_MS)
        {
            backoffMs = MAX_BACKOFF_MS;
            break;
        }
    }
    backoffUntilMs_ = now + backoffMs;
    Serial.print("[HTTP] POST failed code=");
    Serial.print(code);
    Serial.print(" backoff=");
    Serial.print(backoffMs / 1000);
    Serial.println("s");
    return false;
}

String HttpClient::buildJson(float temp, float humi,
                             const EdgeAssessment &assessment)
{
    // 用 millis() 当时间戳占位
    // 后端用 server_timestamp 覆盖，不影响存储
    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = millis() / 1000;
    doc["temperature"] = roundf(temp * 10.0f) / 10.0f;
    doc["humidity"] = roundf(humi * 10.0f) / 10.0f;
    JsonObject edge = doc["edge"].to<JsonObject>();
    edge["state"] = assessment.state;
    edge["severity"] = assessment.severity;
    edge["confidence"] = roundf(assessment.confidence * 100.0f) / 100.0f;
    edge["reason_code"] = assessment.reasonCode;
    edge["reason"] = assessment.reason;

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
#if !ENABLE_BUZZER
    (void)durationMs;
    if (command == "buzzer_on" || command == "buzzer_off")
    {
        Serial.println("[CMD] Buzzer command rejected: hardware disabled");
        return false;
    }
#else
    if (command == "buzzer_on")
    {
        if (durationMs < 0 || durationMs > 30000)
            return false;

        Serial.print("[CMD] buzzer_on ");
        Serial.print(durationMs);
        Serial.println("ms");
        digitalWrite(BUZZER_PIN, BUZZER_ON_LEVEL);
        if (durationMs > 0)
        {
            delay(durationMs);
            digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
        }
        return true;
    }

    if (command == "buzzer_off")
    {
        Serial.println("[CMD] buzzer_off");
        digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
        return true;
    }
#endif

    Serial.print("[CMD] Rejected command: ");
    Serial.println(command);
    return false;
}
