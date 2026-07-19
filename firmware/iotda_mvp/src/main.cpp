#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <time.h>

#include "config.h"
#include "certs.h"

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

unsigned long lastReportMs = 0;
bool simulatedBuzzerOn = false;

static String bytesToHex(const unsigned char *bytes, size_t len)
{
    static const char *hex = "0123456789abcdef";
    String out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        out += hex[(bytes[i] >> 4) & 0x0F];
        out += hex[bytes[i] & 0x0F];
    }
    return out;
}

static String hmacSha256Hex(const String &key, const String &message)
{
    unsigned char digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char *>(key.c_str()), key.length());
    mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char *>(message.c_str()), message.length());
    mbedtls_md_hmac_finish(&ctx, digest);
    mbedtls_md_free(&ctx);
    return bytesToHex(digest, sizeof(digest));
}

// 生成华为云 MQTT 鉴权所需的 UTC 小时时间戳。
static String utcHourTimestamp()
{
    struct tm tmUtc;
    if (!getLocalTime(&tmUtc, 10000))
        return "";

    char buf[11];
    strftime(buf, sizeof(buf), "%Y%m%d%H", &tmUtc);
    return String(buf);
}

// 设备属性上报使用 IoTDA 官方 Topic。
static String propertyReportTopic()
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/properties/report";
}

// 订阅该通配 Topic，接收平台发给当前设备的全部命令。
static String commandSubscribeTopic()
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/commands/#";
}

// 命令响应必须携带下行消息中的原始 request_id。
static String commandResponseTopic(const String &requestId)
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) +
           "/sys/commands/response/request_id=" + requestId;
}

// 从命令 Topic 中提取 request_id，用于匹配命令响应。
static String requestIdFromTopic(const String &topic)
{
    const String marker = "request_id=";
    const int start = topic.indexOf(marker);
    return start < 0 ? "" : topic.substring(start + marker.length());
}

// 按 IoTDA 官方结构回复命令执行成功或失败。
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

    Serial.print("[COMMAND] Response=");
    Serial.println(payload);
    const bool ok = mqtt.publish(topic.c_str(), payload.c_str());
    Serial.println(ok ? "[COMMAND] Response publish OK" : "[COMMAND] Response publish failed");
}

// 兼容产品模型可能下发的布尔值、整数或字符串 0/1。
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

// 校验下行命令，并执行当前的串口蜂鸣器模拟。
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

    // 接入真实蜂鸣器后，在此处替换为 GPIO 或 PWM 控制。
    simulatedBuzzerOn = enabled;
    Serial.print("[BUZZER SIM] ");
    Serial.println(simulatedBuzzerOn ? "ON" : "OFF");

    if (!doc["paras"]["duration_ms"].isNull())
    {
        Serial.print("[BUZZER SIM] duration_ms=");
        Serial.println(doc["paras"]["duration_ms"].as<unsigned long>());
    }
    if (!doc["paras"]["pattern"].isNull())
    {
        Serial.print("[BUZZER SIM] pattern=");
        Serial.println(doc["paras"]["pattern"].as<const char *>());
    }

    publishCommandResponse(requestId, 0, "success");
}

// 使用 IoTDA 官方 services 结构构造模拟温湿度属性。
static String buildPropertyPayload()
{
    JsonDocument doc;
    JsonArray services = doc["services"].to<JsonArray>();
    JsonObject service = services.add<JsonObject>();
    service["service_id"] = IOTDA_SERVICE_ID;

    JsonObject properties = service["properties"].to<JsonObject>();

    // MVP uses test values to activate and verify the cloud path.
    // Replace with SHT30 readings after IoTDA MQTT is confirmed working.
    const float temp = 25.0f + (millis() % 1000) / 1000.0f;
    const float humi = 60.0f + (millis() % 2000) / 200.0f;
    properties["temperature"] = roundf(temp * 10.0f) / 10.0f;
    properties["humidity"] = roundf(humi * 10.0f) / 10.0f;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

// 阻塞等待 Wi-Fi 接通；断线后由 loop 再次调用。
static void connectWiFi()
{
    Serial.print("[WiFi] Connecting to ");
    Serial.println(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("[WiFi] Connected, IP=");
    Serial.println(WiFi.localIP());
}

// TLS 证书校验和 MQTT 鉴权都依赖正确的系统时间。
static void syncTime()
{
    Serial.println("[TIME] Syncing UTC time by NTP...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    String timestamp = utcHourTimestamp();
    if (timestamp.isEmpty())
    {
        Serial.println("[TIME] NTP sync failed; MQTT auth timestamp is unavailable.");
        return;
    }

    Serial.print("[TIME] UTC auth timestamp=");
    Serial.println(timestamp);
}

// 连接 IoTDA，并在每次重连后重新订阅命令 Topic。
static bool connectMqtt()
{
    if (String(IOTDA_MQTT_HOST).startsWith("your-") ||
        String(IOTDA_DEVICE_ID).startsWith("your_") ||
        String(IOTDA_DEVICE_SECRET).startsWith("your_"))
    {
        Serial.println("[MQTT] IoTDA config is still placeholder. Fill src/config.h first.");
        return false;
    }

    Serial.print("[MQTT] Connecting host=");
    Serial.print(IOTDA_MQTT_HOST);
    Serial.print(" port=");
    Serial.println(IOTDA_MQTT_PORT);
    Serial.print("[MQTT] clientId length=");
    Serial.println(String(IOTDA_MQTT_CLIENT_ID).length());
    Serial.print("[MQTT] username length=");
    Serial.println(String(IOTDA_MQTT_USERNAME).length());
    Serial.print("[MQTT] password length=");
    Serial.println(String(IOTDA_MQTT_PASSWORD).length());

    mqtt.setServer(IOTDA_MQTT_HOST, IOTDA_MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(512);
    bool ok = mqtt.connect(IOTDA_MQTT_CLIENT_ID, IOTDA_MQTT_USERNAME, IOTDA_MQTT_PASSWORD);
    if (!ok)
    {
        Serial.print("[MQTT] Connect failed, state=");
        Serial.println(mqtt.state());
        return false;
    }

    Serial.println("[MQTT] Connected to IoTDA.");
    const String topic = commandSubscribeTopic();
    const bool subscribed = mqtt.subscribe(topic.c_str());
    Serial.print("[COMMAND] Subscribe ");
    Serial.print(topic);
    Serial.println(subscribed ? " OK" : " failed");
    return true;
}

// 将当前模拟温湿度数据发布到 IoTDA。
static void publishPropertyReport()
{
    String topic = propertyReportTopic();
    String payload = buildPropertyPayload();

    Serial.print("[IoTDA] Publish topic=");
    Serial.println(topic);
    Serial.print("[IoTDA] Payload=");
    Serial.println(payload);

    bool ok = mqtt.publish(topic.c_str(), payload.c_str());
    Serial.println(ok ? "[IoTDA] Publish OK" : "[IoTDA] Publish failed");
}

void setup()
{
    Serial.begin(115200);
    delay(3000);

    Serial.println();
    Serial.println("=== IoTDA Activation MVP ===");

    connectWiFi();

    tlsClient.setCACert(HUAWEI_ROOT_CA);

    syncTime();
    connectMqtt();
}

// 持续维护网络和 MQTT，并按配置周期上报属性。
void loop()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        connectWiFi();
    }

    if (!mqtt.connected())
    {
        connectMqtt();
    }

    mqtt.loop();

    if (mqtt.connected() && millis() - lastReportMs >= REPORT_INTERVAL_MS)
    {
        lastReportMs = millis();
        publishPropertyReport();
    }

    delay(20);
}
