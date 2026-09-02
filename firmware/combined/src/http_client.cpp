#include "http_client.h"
#include "oled.h"
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

// 触发电平只在 config.h 里定义一次。之前 ON/OFF 两个常量在 main.cpp 和这里
// 各写了一份，改一处漏一处就会出现"命令能响、告警不响"这种难查的问题。
#ifndef BUZZER_ACTIVE_LEVEL
#define BUZZER_ACTIVE_LEVEL LOW
#endif

namespace
{
constexpr uint8_t BUZZER_ON_LEVEL = BUZZER_ACTIVE_LEVEL;
constexpr uint8_t BUZZER_OFF_LEVEL =
    BUZZER_ACTIVE_LEVEL == LOW ? HIGH : LOW;
constexpr unsigned long BUZZER_SELFTEST_MS = 150;
constexpr uint8_t MAX_BACKOFF_FAILURES = 6;
constexpr unsigned long BASE_BACKOFF_MS = 10000;
constexpr unsigned long MAX_BACKOFF_MS = 5UL * 60UL * 1000UL;
}

bool HttpClient::localAlarmActive_ = false;
uint8_t HttpClient::consecutiveFailures_ = 0;
unsigned long HttpClient::backoffUntilMs_ = 0;

void HttpClient::initActuators()
{
#if ENABLE_BUZZER
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
#else
    // Not hardware-verified: keep the GPIO high-impedance so no command can
    // energise it. Nothing is logged here — this runs before Serial.begin().
    pinMode(BUZZER_PIN, INPUT);
#endif
}

void HttpClient::setBuzzer(bool on)
{
#if ENABLE_BUZZER
    digitalWrite(BUZZER_PIN, on ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);
#else
    (void)on;
#endif
}

void HttpClient::selfTestBuzzer()
{
#if ENABLE_BUZZER
    Serial.println("[BUZZER] Self-test beep");
    setBuzzer(true);
    delay(BUZZER_SELFTEST_MS);
    setBuzzer(false);
#endif
}

void HttpClient::setLocalAlarm(bool active)
{
    localAlarmActive_ = active;
}

bool HttpClient::localAlarm()
{
    return localAlarmActive_;
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
            Serial.println("[BACKEND] Recorder recovered; resuming uploads");
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
    // 措辞刻意不写成错误：本地落库是可选目标，它不可达既不影响判决，也不影响
    // 告警和云端上报。写成 "POST failed" 会让串口看起来像系统出了问题。
    Serial.print("[BACKEND] Recorder unreachable (HTTP ");
    Serial.print(code);
    Serial.print("), retry in ");
    Serial.print(backoffMs / 1000);
    Serial.println("s — reasoning, alarm and cloud unaffected");
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
    if ((command == "buzzer_on" || command == "buzzer_off") && localAlarm())
    {
        Serial.println("[CMD] Buzzer command ignored: local alarm active");
        return false;
    }

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
        setBuzzer(true);
        if (durationMs > 0)
        {
            delay(durationMs);
            setBuzzer(false);
        }
        return true;
    }

    if (command == "buzzer_off")
    {
        Serial.println("[CMD] buzzer_off");
        setBuzzer(false);
        return true;
    }
#endif

    if (command.startsWith("oled:"))
    {
        String message = command.substring(5);
        message.trim();
        if (message.isEmpty())
            return false;
        unsigned long dur = durationMs > 0 ? durationMs : 30000;
        bool ok = OLED::showCustomMessage(message, dur);
        Serial.println(ok ? "[CMD] OLED message accepted" : "[CMD] OLED message rejected");
        return ok;
    }

    Serial.print("[CMD] Rejected command: ");
    Serial.println(command);
    return false;
}
