#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "config.h"
#include "certs.h"
#include "wifi_manager.h"
#include "http_client.h"
#include "sht30.h"
#include "oled.h"
#include "median_filter.h"

#ifndef BUZZER_PIN
#define BUZZER_PIN 4
#endif

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

unsigned long lastReportMs = 0;

// ---------------- IoTDA 官方 Topic ----------------
static String propertyReportTopic()
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/properties/report";
}

static String commandSubscribeTopic()
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/commands/#";
}

static String commandResponseTopic(const String &requestId)
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) +
           "/sys/commands/response/request_id=" + requestId;
}

static String requestIdFromTopic(const String &topic)
{
    const String marker = "request_id=";
    const int start = topic.indexOf(marker);
    return start < 0 ? "" : topic.substring(start + marker.length());
}

// 按 IoTDA 官方结构回复命令执行结果。
static void publishCommandResponse(const String &requestId, int resultCode, const char *result)
{
    if (requestId.isEmpty())
    {
        Serial.println("[COMMAND] Missing request_id; response cannot be published.");
        return;
    }

    JsonDocument doc;
    doc["result_code"] = resultCode;
    doc["response_name"] = "COMMAND_RESPONSE";
    doc["paras"]["result"] = result;

    String payload;
    serializeJson(doc, payload);
    const String topic = commandResponseTopic(requestId);

    const bool ok = mqtt.publish(topic.c_str(), payload.c_str());
    Serial.println(ok ? "[COMMAND] Response publish OK" : "[COMMAND] Response publish failed");
}

// 兼容布尔 / 整数 / 字符串 0|1。
static bool parseBuzzerValue(JsonVariantConst value, bool &enabled)
{
    if (value.is<bool>())
    {
        enabled = value.as<bool>();
        return true;
    }
    if (value.is<int>())
    {
        const int number = value.as<int>();
        if (number == 0 || number == 1)
        {
            enabled = number == 1;
            return true;
        }
    }
    if (value.is<const char *>())
    {
        const String text = value.as<const char *>();
        if (text == "0" || text == "1")
        {
            enabled = text == "1";
            return true;
        }
    }
    return false;
}

// 云端下发命令回调：驱动真实蜂鸣器 (BUZZER_PIN)。
static void onMqttMessage(char *topicChars, byte *payload, unsigned int length)
{
    const String topic(topicChars);
    const String requestId = requestIdFromTopic(topic);

    Serial.print("[COMMAND] Topic=");
    Serial.println(topic);

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload, length);
    if (error)
    {
        Serial.print("[COMMAND] Invalid JSON: ");
        Serial.println(error.c_str());
        publishCommandResponse(requestId, 1, "invalid_json");
        return;
    }

    const char *serviceId = doc["service_id"] | "";
    const char *commandName = doc["command_name"] | "";
    if (strcmp(serviceId, IOTDA_ALARM_SERVICE_ID) != 0 ||
        strcmp(commandName, IOTDA_BUZZER_COMMAND_NAME) != 0)
    {
        Serial.println("[COMMAND] Unsupported service or command.");
        publishCommandResponse(requestId, 1, "unsupported_command");
        return;
    }

    bool enabled = false;
    if (!parseBuzzerValue(doc["paras"]["value"], enabled))
    {
        Serial.println("[COMMAND] paras.value must be 0 or 1.");
        publishCommandResponse(requestId, 1, "invalid_value");
        return;
    }

    digitalWrite(BUZZER_PIN, enabled ? HIGH : LOW);
    Serial.print("[BUZZER] ");
    Serial.println(enabled ? "ON" : "OFF");

    const long durationMs = doc["paras"]["duration_ms"] | 0;
    if (enabled && durationMs > 0 && durationMs <= 30000)
    {
        delay(durationMs);
        digitalWrite(BUZZER_PIN, LOW);
    }

    publishCommandResponse(requestId, 0, "success");
}

// TLS 证书校验和 MQTT 鉴权都依赖正确的系统时间。
static String utcHourTimestamp()
{
    struct tm tmUtc;
    if (!getLocalTime(&tmUtc, 10000))
        return "";

    char buf[11];
    strftime(buf, sizeof(buf), "%Y%m%d%H", &tmUtc);
    return String(buf);
}

static void syncTime()
{
    Serial.println("[TIME] Syncing UTC time by NTP...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    const String timestamp = utcHourTimestamp();
    Serial.print("[TIME] UTC auth timestamp=");
    Serial.println(timestamp.isEmpty() ? "(failed)" : timestamp);
}

// 连接 IoTDA，并在每次重连后重新订阅命令 Topic。
static bool connectMqtt()
{
    if (String(IOTDA_MQTT_HOST).startsWith("your-") ||
        String(IOTDA_DEVICE_ID).startsWith("your_"))
    {
        Serial.println("[MQTT] IoTDA config is still placeholder. Fill src/config.h first.");
        return false;
    }

    Serial.print("[MQTT] Connecting host=");
    Serial.print(IOTDA_MQTT_HOST);
    Serial.print(" port=");
    Serial.println(IOTDA_MQTT_PORT);

    mqtt.setServer(IOTDA_MQTT_HOST, IOTDA_MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(512);

    const bool ok = mqtt.connect(IOTDA_MQTT_CLIENT_ID, IOTDA_MQTT_USERNAME, IOTDA_MQTT_PASSWORD);
    if (!ok)
    {
        Serial.print("[MQTT] Connect failed, state=");
        Serial.println(mqtt.state());
        return false;
    }

    Serial.println("[MQTT] Connected to IoTDA.");
    OLED::showStatus("MQTT OK");
    const String topic = commandSubscribeTopic();
    const bool subscribed = mqtt.subscribe(topic.c_str());
    Serial.print("[COMMAND] Subscribe ");
    Serial.print(topic);
    Serial.println(subscribed ? " OK" : " failed");
    return true;
}

// 用 IoTDA 官方 services 结构构造真实温湿度属性。
static String buildPropertyPayload(float temp, float humi)
{
    JsonDocument doc;
    JsonArray services = doc["services"].to<JsonArray>();
    JsonObject service = services.add<JsonObject>();
    service["service_id"] = IOTDA_SERVICE_ID;

    JsonObject properties = service["properties"].to<JsonObject>();
    properties["temperature"] = roundf(temp * 10.0f) / 10.0f;
    properties["humidity"] = roundf(humi * 10.0f) / 10.0f;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

static void publishToCloud(float temp, float humi)
{
    if (!mqtt.connected())
        return;

    const String topic = propertyReportTopic();
    const String payload = buildPropertyPayload(temp, humi);

    Serial.print("[IoTDA] Publish payload=");
    Serial.println(payload);

    const bool ok = mqtt.publish(topic.c_str(), payload.c_str());
    Serial.println(ok ? "[IoTDA] Publish OK" : "[IoTDA] Publish failed");
}

void setup()
{
    Serial.begin(115200);
    delay(3000);

    Serial.println();
    Serial.println("=== Edge IoT Monitor (Combined: local + cloud) ===");

    HttpClient::initActuators();

    OLED::init();
    OLED::showStatus("STARTING");

    OLED::showStatus("WIFI CONNECTING");
    if (!WiFiManager::init(WIFI_SSID, WIFI_PASS, 20000))
    {
        Serial.println("[ERROR] WiFi failed");
        OLED::showStatus("WIFI FAILED");
        while (true) delay(1000);
    }
    Serial.println("[OK] WiFi connected");

    if (!SHT30::init())
    {
        Serial.println("[ERROR] SHT30 failed");
        OLED::showStatus("SHT30 FAILED");
        while (true) delay(1000);
    }
    Serial.println("[OK] SHT30 ready");

    // 云端准备：TLS 根证书 + NTP 对时 + MQTT 连接。
    tlsClient.setCACert(HUAWEI_ROOT_CA);

    OLED::showStatus("TIME SYNC");
    syncTime();

    OLED::showStatus("MQTT CONNECTING");
    connectMqtt();
}

void loop()
{
    WiFiManager::reconnectIfNeeded();

    // 保持 MQTT 在线并及时处理下行命令。
    if (!mqtt.connected())
        connectMqtt();
    mqtt.loop();

    if (millis() - lastReportMs < REPORT_INTERVAL_MS)
    {
        delay(20);
        return;
    }
    lastReportMs = millis();

    float temp = 0, humi = 0;
    if (!SHT30::read(temp, humi))
    {
        Serial.println("[WARN] SHT30 read failed");
        return;
    }

    temp = medianFilterTemp(temp);
    humi = medianFilterHumi(humi);

    Serial.print("[SENSOR] Temp=");
    Serial.print(temp, 1);
    Serial.print("C  Humi=");
    Serial.print(humi, 1);
    Serial.println("%");

    OLED::showSensorData(temp, humi);

    // 本地后端路径：上报数据 + 拉取并执行本地命令。
    HttpClient::postSensorData(temp, humi);
    HttpClient::pollAndApplyCommand();

    // 云端路径：上报到华为云 IoTDA（连接状态在 connectMqtt 成功时已显示到 OLED）。
    publishToCloud(temp, humi);
}
