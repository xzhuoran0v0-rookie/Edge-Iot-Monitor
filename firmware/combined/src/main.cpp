#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Preferences.h>
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
#include "adaptive_baseline.h"
#include "edge_reasoner.h"

#ifndef BUZZER_PIN
#define BUZZER_PIN 4
#endif

#ifndef ENABLE_BUZZER
#define ENABLE_BUZZER 0
#endif

#ifndef IOTDA_EDGE_SERVICE_ID
#define IOTDA_EDGE_SERVICE_ID "EdgeReasoning"
#endif

#ifndef IOTDA_DISPLAY_SERVICE_ID
#define IOTDA_DISPLAY_SERVICE_ID "Display"
#endif

#ifndef IOTDA_DISPLAY_COMMAND_NAME
#define IOTDA_DISPLAY_COMMAND_NAME "ShowMessage"
#endif

namespace
{
constexpr uint8_t BUZZER_ON_LEVEL = LOW;
constexpr uint8_t BUZZER_OFF_LEVEL = HIGH;
constexpr unsigned long SENSOR_RETRY_INTERVAL_MS = 2000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long SAFETY_SAMPLE_INTERVAL_MS = 1000;
constexpr unsigned long SAFETY_SAMPLE_MAX_AGE_MS = 1500;
constexpr uint8_t HARD_LIMIT_CLEAR_SAFE_SAMPLES = 3;
constexpr size_t MQTT_BUFFER_SIZE = 1024;
constexpr unsigned long BASELINE_PERSIST_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 8000;
constexpr unsigned long NTP_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr uint8_t DRIFT_WINDOW_SIZE = 10;
constexpr float DRIFT_MIN_TEMP_RISE_C = 2.0f;
constexpr float DRIFT_MAX_HUMIDITY_RISE_PCT = 1.0f;
constexpr char BASELINE_NVS_NAMESPACE[] = "envbaseline";
constexpr char BASELINE_NVS_KEY[] = "state";

AdaptiveBaselineConfig makeAdaptiveBaselineConfig()
{
    AdaptiveBaselineConfig config;
    // Save once when learning first completes, then at most about once per
    // hour at the current 10-second reasoning interval.
    config.persistEveryLearnedSamples = 360;
    config.minPersistIntervalMs = 60UL * 60UL * 1000UL;
    return config;
}

bool sensorReady = false;
unsigned long lastSensorRetryMs = 0;
unsigned long lastMqttAttemptMs = 0;
EdgeReasoner edgeReasoner;
AdaptiveBaseline adaptiveBaseline(makeAdaptiveBaselineConfig());
AdaptiveBaselineResult latestBaseline;
Preferences baselinePreferences;
bool baselinePreferencesReady = false;
bool baselinePersistAttempted = false;
unsigned long lastBaselinePersistAttemptMs = 0;
bool invalidSampleAlert = false;
bool hardLimitAlert = false;
uint8_t hardLimitSafeSamples = 0;
bool ntpSynced = false;
unsigned long lastNtpAttemptMs = 0;
bool backendWasReachable = true;
bool mqttWasConnected = false;
bool wifiWasConnected = false;

struct DriftSample
{
    float temperature = 0.0f;
    float humidity = 0.0f;
};
DriftSample driftWindow[DRIFT_WINDOW_SIZE]{};
uint8_t driftCount = 0;
uint8_t driftNext = 0;
bool sensorDriftAlert = false;

struct SafetySnapshot
{
    bool valid = false;
    bool hardLimit = false;
    float temperature = 0.0f;
    float humidity = 0.0f;
    uint32_t sampledAtMs = 0;
    uint32_t generation = 0;
};

struct PendingHardLimitEvent
{
    bool pending = false;
    float temperature = 0.0f;
    float humidity = 0.0f;
    uint32_t generation = 0;
};

portMUX_TYPE safetyStateMux = portMUX_INITIALIZER_UNLOCKED;
SafetySnapshot latestSafetySnapshot;
PendingHardLimitEvent pendingHardLimitEvent;
TaskHandle_t safetyTaskHandle = nullptr;
bool safetyTaskStarted = false;
}

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

unsigned long lastSafetySampleMs = 0;
unsigned long lastReasoningReportMs = 0;
unsigned long lastCloudReportMs = 0;

static void initAdaptiveBaseline()
{
    baselinePreferencesReady =
        baselinePreferences.begin(BASELINE_NVS_NAMESPACE, false);
    if (!baselinePreferencesReady)
    {
        Serial.println("[BASELINE] NVS unavailable; learning remains in RAM");
        return;
    }

    const size_t storedBytes =
        baselinePreferences.getBytesLength(BASELINE_NVS_KEY);
    if (storedBytes == 0)
    {
        Serial.println("[BASELINE] No saved state; starting learning");
        return;
    }
    if (storedBytes != AdaptiveBaseline::persistentStateSize())
    {
        Serial.println("[BASELINE] Saved state size mismatch; relearning");
        return;
    }

    AdaptiveBaselinePersistentState state;
    const size_t loadedBytes = baselinePreferences.getBytes(
        BASELINE_NVS_KEY, &state, sizeof(state));
    if (loadedBytes == sizeof(state) &&
        adaptiveBaseline.restoreState(state, millis()))
    {
        latestBaseline = adaptiveBaseline.snapshot();
        Serial.print("[BASELINE] Restored samples=");
        Serial.println(latestBaseline.learnedSamples);
        return;
    }

    Serial.println("[BASELINE] Saved state invalid; relearning");
}

static void persistAdaptiveBaselineIfDue()
{
    const unsigned long now = millis();
    if (!baselinePreferencesReady ||
        !adaptiveBaseline.persistenceDue(now) ||
        (baselinePersistAttempted &&
         now - lastBaselinePersistAttemptMs < BASELINE_PERSIST_RETRY_MS))
    {
        return;
    }

    baselinePersistAttempted = true;
    lastBaselinePersistAttemptMs = now;
    const AdaptiveBaselinePersistentState state =
        adaptiveBaseline.exportState();
    const size_t written = baselinePreferences.putBytes(
        BASELINE_NVS_KEY, &state, sizeof(state));
    if (written != sizeof(state))
    {
        Serial.println("[BASELINE] NVS save failed");
        return;
    }

    adaptiveBaseline.markPersisted(now);
    Serial.print("[BASELINE] Saved samples=");
    Serial.println(state.learnedSamples);
}

static void addDriftSample(float temperature, float humidity)
{
    driftWindow[driftNext].temperature = temperature;
    driftWindow[driftNext].humidity = humidity;
    driftNext = (driftNext + 1) % DRIFT_WINDOW_SIZE;
    if (driftCount < DRIFT_WINDOW_SIZE)
        ++driftCount;
}

static bool detectSensorDrift()
{
    if (driftCount < DRIFT_WINDOW_SIZE)
        return false;

    const uint8_t oldest = driftNext;
    const uint8_t newest = (driftNext + DRIFT_WINDOW_SIZE - 1) % DRIFT_WINDOW_SIZE;
    const float tempRise =
        driftWindow[newest].temperature - driftWindow[oldest].temperature;
    const float humiRise =
        driftWindow[newest].humidity - driftWindow[oldest].humidity;

    if (tempRise < DRIFT_MIN_TEMP_RISE_C || humiRise > DRIFT_MAX_HUMIDITY_RISE_PCT)
        return false;

    for (uint8_t i = 1; i < driftCount; ++i)
    {
        const uint8_t prev = (oldest + i - 1) % DRIFT_WINDOW_SIZE;
        const uint8_t curr = (oldest + i) % DRIFT_WINDOW_SIZE;
        if (driftWindow[curr].temperature < driftWindow[prev].temperature - 0.1f)
            return false;
    }
    return true;
}

static bool trySyncTime()
{
    Serial.println("[TIME] Syncing UTC time by NTP...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    struct tm tmUtc;
    if (!getLocalTime(&tmUtc, NTP_SYNC_TIMEOUT_MS))
    {
        Serial.println("[TIME] NTP sync failed; MQTT deferred");
        return false;
    }

    char buf[11];
    strftime(buf, sizeof(buf), "%Y%m%d%H", &tmUtc);
    Serial.print("[TIME] UTC auth timestamp=");
    Serial.println(buf);
    return true;
}

static void retryNtpIfNeeded()
{
    if (ntpSynced || !WiFiManager::isConnected())
        return;

    const unsigned long now = millis();
    if (now - lastNtpAttemptMs < NTP_RETRY_INTERVAL_MS)
        return;

    lastNtpAttemptMs = now;
    if (trySyncTime())
    {
        ntpSynced = true;
        OLED::showStatus("TIME OK");
    }
}

static EdgeAssessment applyAdaptiveAssessment(
    const EdgeAssessment &fixedAssessment,
    const AdaptiveBaselineResult &baseline)
{
    if (baseline.hardLimitExceeded)
    {
        return {"HARD_LIMIT", "warning", 0.95f,
                "FIXED_SAFETY_LIMIT",
                "A fixed environmental safety limit was crossed.",
                "SAFETY LIMIT"};
    }

    const bool fixedCanYield =
        strcmp(fixedAssessment.state, "NORMAL") == 0 ||
        (baseline.ready &&
         strcmp(fixedAssessment.state, "WARMUP") == 0);
    if (!baseline.outsideAdaptiveBand || !fixedCanYield)
    {
        return fixedAssessment;
    }

    if (baseline.temperatureOutsideBand &&
        baseline.humidityOutsideBand)
    {
        return {"BASELINE_SHIFT", "watch", 0.82f,
                "TEMP_HUMIDITY_OUTSIDE_BASELINE",
                "Temperature and humidity moved outside the learned range.",
                "PATTERN SHIFT"};
    }
    if (baseline.temperatureOutsideBand)
    {
        return {"BASELINE_SHIFT", "watch", 0.78f,
                "TEMP_OUTSIDE_BASELINE",
                "Temperature moved outside the learned normal range.",
                "TEMP SHIFT"};
    }
    return {"BASELINE_SHIFT", "watch", 0.78f,
            "HUMIDITY_OUTSIDE_BASELINE",
            "Humidity moved outside the learned normal range.",
            "HUMI SHIFT"};
}

static bool isPhysicalSensorSampleValid(float temperature,
                                        float humidity)
{
    const AdaptiveBaselineConfig &config = adaptiveBaseline.config();
    return isfinite(temperature) &&
           isfinite(humidity) &&
           temperature >= config.physicalTempLowerC &&
           temperature <= config.physicalTempUpperC &&
           humidity >= config.physicalHumidityLowerPct &&
           humidity <= config.physicalHumidityUpperPct;
}

static AdaptiveBaselineResult makeRawHardLimitResult(
    float temperature,
    float humidity)
{
    const AdaptiveBaselineConfig &config = adaptiveBaseline.config();
    const bool temperatureHard =
        temperature <= config.hardTempLowerC ||
        temperature >= config.hardTempUpperC;
    const bool humidityHard =
        humidity <= config.hardHumidityLowerPct ||
        humidity >= config.hardHumidityUpperPct;

    AdaptiveBaselineResult result = adaptiveBaseline.snapshot();
    result.status = AdaptiveBaselineStatus::HARD_LIMIT;
    result.validSample = true;
    result.learned = false;
    result.hardLimitExceeded = temperatureHard || humidityHard;
    result.temperatureHardLimit = temperatureHard;
    result.humidityHardLimit = humidityHard;
    return result;
}

static void storeSafetySnapshot(bool valid,
                                bool hardLimit,
                                float temperature,
                                float humidity)
{
    portENTER_CRITICAL(&safetyStateMux);
    if (latestSafetySnapshot.valid != valid ||
        (valid &&
         latestSafetySnapshot.hardLimit != hardLimit))
    {
        ++latestSafetySnapshot.generation;
    }
    latestSafetySnapshot.valid = valid;
    latestSafetySnapshot.hardLimit = hardLimit;
    latestSafetySnapshot.temperature = temperature;
    latestSafetySnapshot.humidity = humidity;
    latestSafetySnapshot.sampledAtMs = millis();
    portEXIT_CRITICAL(&safetyStateMux);
}

static SafetySnapshot loadSafetySnapshot()
{
    portENTER_CRITICAL(&safetyStateMux);
    const SafetySnapshot snapshot = latestSafetySnapshot;
    portEXIT_CRITICAL(&safetyStateMux);
    return snapshot;
}

static bool safetySnapshotStillCurrent(
    const SafetySnapshot &snapshot)
{
    const SafetySnapshot current = loadSafetySnapshot();
    const unsigned long now = millis();
    return snapshot.valid &&
           current.valid &&
           current.generation == snapshot.generation &&
           now - snapshot.sampledAtMs <=
               SAFETY_SAMPLE_MAX_AGE_MS &&
           now - current.sampledAtMs <=
               SAFETY_SAMPLE_MAX_AGE_MS;
}

static void queueHardLimitEvent(float temperature,
                                float humidity)
{
    portENTER_CRITICAL(&safetyStateMux);
    pendingHardLimitEvent.pending = true;
    pendingHardLimitEvent.temperature = temperature;
    pendingHardLimitEvent.humidity = humidity;
    ++pendingHardLimitEvent.generation;
    portEXIT_CRITICAL(&safetyStateMux);
}

static PendingHardLimitEvent loadPendingHardLimitEvent()
{
    portENTER_CRITICAL(&safetyStateMux);
    const PendingHardLimitEvent event = pendingHardLimitEvent;
    portEXIT_CRITICAL(&safetyStateMux);
    return event;
}

static void acknowledgeHardLimitEvent(uint32_t generation)
{
    portENTER_CRITICAL(&safetyStateMux);
    if (pendingHardLimitEvent.generation == generation)
        pendingHardLimitEvent.pending = false;
    portEXIT_CRITICAL(&safetyStateMux);
}

static void blockInvalidSensorSample(float temperature,
                                     float humidity)
{
    invalidSampleAlert = true;
    hardLimitAlert = false;
    hardLimitSafeSamples = 0;
    storeSafetySnapshot(false, false, temperature, humidity);
    Serial.print("[SHT30] Invalid physical sample; uploads blocked temp=");
    Serial.print(temperature);
    Serial.print(" humidity=");
    Serial.println(humidity);
    OLED::showAlert("SHT30 INVALID DATA");
}

static bool ensureSensorReady(bool force = false)
{
    if (sensorReady)
        return true;

    const unsigned long now = millis();
    if (!force && now - lastSensorRetryMs < SENSOR_RETRY_INTERVAL_MS)
        return false;

    lastSensorRetryMs = now;
    Serial.println("[SHT30] Trying to connect...");
    if (!SHT30::init())
    {
        Serial.println("[SHT30] Offline; local and cloud uploads blocked");
        OLED::showAlert("SHT30 OFFLINE");
        return false;
    }

    sensorReady = true;
    Serial.println("[SHT30] Reconnected");
    OLED::clearAlert();
    OLED::showStatus("SHT30 ONLINE");
    return true;
}

static void markSensorOffline()
{
    sensorReady = false;
    invalidSampleAlert = false;
    hardLimitAlert = false;
    hardLimitSafeSamples = 0;
    sensorDriftAlert = false;
    driftCount = 0;
    driftNext = 0;
    storeSafetySnapshot(false, false, 0.0f, 0.0f);
    lastSensorRetryMs = millis();
    Serial.println("[SHT30] Lost connection; local and cloud uploads blocked");
    OLED::showAlert("SHT30 OFFLINE");
}

static void sampleSafetySensor()
{
    if (!ensureSensorReady())
    {
        storeSafetySnapshot(false, false, 0.0f, 0.0f);
        return;
    }

    float temperature = 0.0f;
    float humidity = 0.0f;
    if (!SHT30::read(temperature, humidity))
    {
        markSensorOffline();
        return;
    }

    if (!isPhysicalSensorSampleValid(temperature, humidity))
    {
        blockInvalidSensorSample(temperature, humidity);
        return;
    }

    if (invalidSampleAlert)
    {
        invalidSampleAlert = false;
        OLED::clearAlert();
    }

    addDriftSample(temperature, humidity);

    const AdaptiveBaselineConfig &config = adaptiveBaseline.config();
    const bool temperatureHard =
        temperature <= config.hardTempLowerC ||
        temperature >= config.hardTempUpperC;
    const bool humidityHard =
        humidity <= config.hardHumidityLowerPct ||
        humidity >= config.hardHumidityUpperPct;
    const bool hardLimit = temperatureHard || humidityHard;

    if (hardLimit && detectSensorDrift())
    {
        storeSafetySnapshot(true, false, temperature, humidity);
        if (!sensorDriftAlert)
        {
            sensorDriftAlert = true;
            hardLimitAlert = false;
            hardLimitSafeSamples = 0;
            OLED::showAlert("SENSOR DRIFT");
            Serial.println("[SAFETY] Sensor self-heating drift detected");
        }
        return;
    }

    if (sensorDriftAlert && !hardLimit)
    {
        sensorDriftAlert = false;
        OLED::clearAlert();
        Serial.println("[SAFETY] Sensor drift cleared");
    }

    storeSafetySnapshot(true, hardLimit, temperature, humidity);

    if (hardLimit)
    {
        sensorDriftAlert = false;
        hardLimitSafeSamples = 0;
        if (!hardLimitAlert)
        {
            hardLimitAlert = true;
            queueHardLimitEvent(temperature, humidity);
            OLED::showAlert("ENV HARD LIMIT");
            Serial.println(
                "[SAFETY] Hard limit latched for next cloud slot");
        }
        return;
    }

    if (!hardLimitAlert)
    {
        hardLimitSafeSamples = 0;
        return;
    }

    if (hardLimitSafeSamples < HARD_LIMIT_CLEAR_SAFE_SAMPLES)
        ++hardLimitSafeSamples;
    if (hardLimitSafeSamples >= HARD_LIMIT_CLEAR_SAFE_SAMPLES)
    {
        hardLimitAlert = false;
        hardLimitSafeSamples = 0;
        OLED::clearAlert();
        Serial.println("[SAFETY] Hard limit cleared after stable safe samples");
    }
}

static void safetyTask(void *)
{
    TickType_t nextWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(
            &nextWake,
            pdMS_TO_TICKS(SAFETY_SAMPLE_INTERVAL_MS));
        sampleSafetySensor();
    }
}

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

// 云端下发命令回调：显示消息或驱动真实蜂鸣器 (BUZZER_PIN)。
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

    if (strcmp(serviceId, IOTDA_DISPLAY_SERVICE_ID) == 0 &&
        strcmp(commandName, IOTDA_DISPLAY_COMMAND_NAME) == 0)
    {
        if (!doc["paras"]["message"].is<const char *>())
        {
            Serial.println("[COMMAND] Display message missing.");
            publishCommandResponse(requestId, 1, "invalid_message");
            return;
        }

        const String message =
            doc["paras"]["message"].as<const char *>();
        const long durationMs =
            doc["paras"]["duration_ms"] | 30000L;
        const bool accepted =
            durationMs >= 0 &&
            OLED::showCustomMessage(
                message, static_cast<unsigned long>(durationMs));
        Serial.println(accepted
                           ? "[COMMAND] Display message accepted"
                           : "[COMMAND] Display message rejected");
        publishCommandResponse(
            requestId,
            accepted ? 0 : 1,
            accepted ? "success" : "display_rejected");
        return;
    }

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

#if !ENABLE_BUZZER
    (void)enabled;
    Serial.println("[BUZZER] Cloud command rejected: hardware disabled");
    publishCommandResponse(requestId, 1, "hardware_disabled");
    return;
#else
    digitalWrite(BUZZER_PIN, enabled ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);
    Serial.print("[BUZZER] ");
    Serial.println(enabled ? "ON" : "OFF");

    const long durationMs = doc["paras"]["duration_ms"] | 0;
    if (enabled && durationMs > 0 && durationMs <= 30000)
    {
        delay(durationMs);
        digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
    }
#endif

    publishCommandResponse(requestId, 0, "success");
}

// syncTime / utcHourTimestamp removed — replaced by trySyncTime() + retryNtpIfNeeded()

// 连接 IoTDA，并在每次重连后重新订阅命令 Topic。
static bool connectMqtt()
{
    lastMqttAttemptMs = millis();
    if (!WiFiManager::isConnected())
        return false;

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
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    const bool ok = mqtt.connect(IOTDA_MQTT_CLIENT_ID, IOTDA_MQTT_USERNAME, IOTDA_MQTT_PASSWORD);
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

// One IoTDA property report carries both services, so a 10-second cloud
// interval consumes 8,640 messages/day instead of 17,280.
static String buildCloudPayload(float temp, float humi,
                                const EdgeAssessment &assessment)
{
    JsonDocument doc;
    JsonArray services = doc["services"].to<JsonArray>();
    JsonObject service = services.add<JsonObject>();
    service["service_id"] = IOTDA_SERVICE_ID;

    JsonObject properties = service["properties"].to<JsonObject>();
    properties["temperature"] = roundf(temp * 10.0f) / 10.0f;
    properties["humidity"] = roundf(humi * 10.0f) / 10.0f;

    JsonObject edgeService = services.add<JsonObject>();
    edgeService["service_id"] = IOTDA_EDGE_SERVICE_ID;
    JsonObject edgeProperties = edgeService["properties"].to<JsonObject>();
    edgeProperties["state"] = assessment.state;
    edgeProperties["severity"] = assessment.severity;
    edgeProperties["confidence"] =
        roundf(assessment.confidence * 100.0f) / 100.0f;
    edgeProperties["reason_code"] = assessment.reasonCode;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

static bool publishToCloud(float temp, float humi,
                           const EdgeAssessment &assessment)
{
    if (!mqtt.connected())
        return false;

    const String topic = propertyReportTopic();
    const String payload = buildCloudPayload(temp, humi, assessment);
    if (payload.length() + topic.length() + 16U >
        MQTT_BUFFER_SIZE)
    {
        Serial.println("[IoTDA] Combined payload exceeds MQTT buffer");
        return false;
    }
    Serial.print("[IoTDA] Combined payload=");
    Serial.println(payload);
    const bool ok = mqtt.publish(topic.c_str(), payload.c_str());
    Serial.println(ok
        ? "[IoTDA] Combined publish OK"
        : "[IoTDA] Combined publish failed");
    return ok;
}

void setup()
{
    Serial.begin(115200);
    delay(3000);

    Serial.println();
    Serial.println("=== Edge IoT Monitor (Combined: local + cloud) ===");
    Serial.printf("[HW] Flash=%u bytes PSRAM=%u bytes\n",
                  ESP.getFlashChipSize(),
                  ESP.getPsramSize());

    initAdaptiveBaseline();
    HttpClient::initActuators();

    OLED::init();
    OLED::showStatus("STARTING");

    // Sensor health is independent of network availability.
    ensureSensorReady(true);
    sampleSafetySensor();
    lastSafetySampleMs = millis();
    const BaseType_t safetyTaskResult =
        xTaskCreatePinnedToCore(
            safetyTask,
            "sht30-safety",
            4096,
            nullptr,
            3,
            &safetyTaskHandle,
            1);
    safetyTaskStarted = safetyTaskResult == pdPASS;
    Serial.println(safetyTaskStarted
                       ? "[SAFETY] Dedicated 1 Hz task started"
                       : "[SAFETY] Task start failed; loop fallback active");

    OLED::showStatus("WIFI CONNECTING");
    if (!WiFiManager::init(WIFI_SSID, WIFI_PASS, 20000))
    {
        Serial.println("[WiFi] Offline; network uploads unavailable");
        OLED::showStatus(
            loadSafetySnapshot().valid
                ? "WIFI OFFLINE"
                : "SHT30 OFFLINE");
    }
    else
    {
        Serial.println("[OK] WiFi connected");
    }

    // 云端准备：TLS 根证书 + NTP 对时 + MQTT 连接。
    tlsClient.setCACert(HUAWEI_ROOT_CA);

    if (WiFiManager::isConnected())
    {
        wifiWasConnected = true;
        OLED::showStatus("TIME SYNC");
        lastNtpAttemptMs = millis();
        ntpSynced = trySyncTime();

        if (ntpSynced)
        {
            OLED::showStatus("MQTT CONNECTING");
            mqttWasConnected = connectMqtt();
        }
        else
        {
            OLED::showStatus("NTP FAILED");
        }
    }
}

void loop()
{
    OLED::tick();

    // Safety sampling always runs before network maintenance. Cloud quota
    // policy is never allowed to extend this one-second local interval.
    const unsigned long safetyNow = millis();
    if (!safetyTaskStarted &&
        safetyNow - lastSafetySampleMs >=
            SAFETY_SAMPLE_INTERVAL_MS)
    {
        lastSafetySampleMs = safetyNow;
        sampleSafetySensor();
    }

    WiFiManager::reconnectIfNeeded();
    const bool wifiNow = WiFiManager::isConnected();
    if (wifiNow != wifiWasConnected)
    {
        wifiWasConnected = wifiNow;
        if (!wifiNow)
            OLED::showStatus("WIFI OFFLINE");
        else
            OLED::showStatus("WIFI OK");
    }

    if (wifiNow)
    {
        retryNtpIfNeeded();

        if (ntpSynced)
        {
            const bool mqttNow = mqtt.connected();
            if (!mqttNow && millis() - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS)
            {
                const bool ok = connectMqtt();
                if (ok && !mqttWasConnected)
                    OLED::showStatus("MQTT OK");
                if (!ok && mqttWasConnected)
                    OLED::showStatus("MQTT OFFLINE");
                mqttWasConnected = ok;
            }
            else if (mqttNow != mqttWasConnected)
            {
                mqttWasConnected = mqttNow;
                OLED::showStatus(mqttNow ? "MQTT OK" : "MQTT OFFLINE");
            }
            if (mqtt.connected())
                mqtt.loop();
        }
    }

    // A hard-limit transition is latched until the next available 10-second
    // cloud slot. The local alert is immediate, while cloud traffic remains
    // inside the daily quota even if the condition clears before that slot.
    const PendingHardLimitEvent pendingEvent =
        loadPendingHardLimitEvent();
    if (pendingEvent.pending &&
        mqtt.connected() &&
        millis() - lastCloudReportMs >=
            REPORT_INTERVAL_MS)
    {
        lastCloudReportMs = millis();
        const AdaptiveBaselineResult pendingLimit =
            makeRawHardLimitResult(
                pendingEvent.temperature,
                pendingEvent.humidity);
        const EdgeAssessment pendingAssessment =
            applyAdaptiveAssessment(
                edgeReasoner.assess(), pendingLimit);
        if (publishToCloud(
                pendingEvent.temperature,
                pendingEvent.humidity,
                pendingAssessment))
        {
            acknowledgeHardLimitEvent(
                pendingEvent.generation);
            Serial.println("[SAFETY] Latched hard limit published");
        }
    }

    const unsigned long reportNow = millis();
    if (reportNow - lastReasoningReportMs < REPORT_INTERVAL_MS)
    {
        delay(20);
        return;
    }

    // Invalid/offline sensor states revoke the cached report immediately.
    // A timestamp guard also blocks a stale value if the safety task stalls.
    const SafetySnapshot safetySnapshot =
        loadSafetySnapshot();
    const unsigned long snapshotNow = millis();
    if (!safetySnapshot.valid ||
        snapshotNow - safetySnapshot.sampledAtMs >
            SAFETY_SAMPLE_MAX_AGE_MS)
    {
        if (safetySnapshot.valid)
            Serial.println("[SAFETY] Stale sample; reports blocked");
        delay(20);
        return;
    }
    lastReasoningReportMs = reportNow;

    float temp = safetySnapshot.temperature;
    float humi = safetySnapshot.humidity;
    if (safetySnapshot.hardLimit)
    {
        latestBaseline =
            makeRawHardLimitResult(temp, humi);
    }
    else
    {
        temp = medianFilterTemp(temp);
        humi = medianFilterHumi(humi);
        latestBaseline = adaptiveBaseline.observe(temp, humi);
        if (!latestBaseline.validSample)
        {
            Serial.println(
                "[BASELINE] Filtered sample invalid; report blocked");
            return;
        }

        edgeReasoner.add(temp, humi, millis());
    }

    persistAdaptiveBaselineIfDue();
    const EdgeAssessment assessment =
        applyAdaptiveAssessment(edgeReasoner.assess(), latestBaseline);

    Serial.print("[SENSOR] Temp=");
    Serial.print(temp, 1);
    Serial.print("C  Humi=");
    Serial.print(humi, 1);
    Serial.println("%");
    Serial.print("[EDGE] state=");
    Serial.print(assessment.state);
    Serial.print(" severity=");
    Serial.print(assessment.severity);
    Serial.print(" confidence=");
    Serial.print(assessment.confidence, 2);
    Serial.print(" reason=");
    Serial.println(assessment.reasonCode);
    Serial.print("[BASELINE] status=");
    Serial.print(AdaptiveBaseline::statusName(latestBaseline.status));
    Serial.print(" progress=");
    Serial.print(latestBaseline.progressPct);
    Serial.print("% samples=");
    Serial.println(latestBaseline.learnedSamples);

    OLED::updateDashboard(
        temp, humi, assessment, latestBaseline, mqtt.connected());

    // Exactly one combined MQTT message carries both services each routine
    // cycle: 8,640 reports/day at the configured 10-second interval.
    if (!loadPendingHardLimitEvent().pending &&
        mqtt.connected() &&
        reportNow - lastCloudReportMs >= REPORT_INTERVAL_MS &&
        safetySnapshotStillCurrent(safetySnapshot))
    {
        lastCloudReportMs = reportNow;
        publishToCloud(temp, humi, assessment);
    }

    if (!safetySnapshotStillCurrent(safetySnapshot))
    {
        Serial.println(
            "[SAFETY] Sensor state changed; uploads blocked");
        return;
    }
    HttpClient::postSensorData(temp, humi, assessment);

    const bool backendNow = HttpClient::backendReachable();
    if (backendNow != backendWasReachable)
    {
        backendWasReachable = backendNow;
        OLED::showStatus(backendNow ? "BACKEND OK" : "BACKEND OFFLINE");
    }

    if (backendNow)
        HttpClient::pollAndApplyCommand();
}
