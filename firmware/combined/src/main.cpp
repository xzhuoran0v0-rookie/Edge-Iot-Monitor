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

// 本地阈值告警。刻意低于 AdaptiveBaseline 的硬限（45C / 95%RH），
// 硬限是安全边界，这个是演示和日常可达的告警点。
// 旧配置只有一个 REPORT_INTERVAL_MS，两者都从它推导，保持向后兼容。
#ifndef SENSE_INTERVAL_MS
#ifdef REPORT_INTERVAL_MS
#define SENSE_INTERVAL_MS REPORT_INTERVAL_MS
#else
#define SENSE_INTERVAL_MS 2000
#endif
#endif

#ifndef CLOUD_INTERVAL_MS
#ifdef REPORT_INTERVAL_MS
#define CLOUD_INTERVAL_MS REPORT_INTERVAL_MS
#else
#define CLOUD_INTERVAL_MS 60000
#endif
#endif

#ifndef ALARM_TEMP_C
#define ALARM_TEMP_C 30.0f
#endif

#ifndef ALARM_HUMIDITY_PCT
#define ALARM_HUMIDITY_PCT 80.0f
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
constexpr uint8_t ALARM_CLEAR_SAFE_SAMPLES = 3;
constexpr unsigned long SENSOR_RETRY_INTERVAL_MS = 2000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long SAFETY_SAMPLE_INTERVAL_MS = 1000;
constexpr unsigned long SAFETY_SAMPLE_MAX_AGE_MS = 1500;
constexpr uint8_t HARD_LIMIT_CLEAR_SAFE_SAMPLES = 3;
constexpr size_t MQTT_BUFFER_SIZE = 1024;
constexpr unsigned long BASELINE_PERSIST_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 8000;
constexpr unsigned long NTP_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;
// 趋势窗口时长 = EdgeReasoner::WINDOW_SIZE × 本值 = 12 × 10 s = 2 min，
// 与 edge_reasoner.cpp 中阈值的整定条件一致。改感知周期不影响它。
constexpr unsigned long EDGE_SAMPLE_INTERVAL_MS = 10000;
constexpr uint8_t DRIFT_WINDOW_SIZE = 10;
constexpr float DRIFT_MIN_TEMP_RISE_C = 2.0f;
constexpr float DRIFT_MAX_HUMIDITY_RISE_PCT = 1.0f;
constexpr char BASELINE_NVS_NAMESPACE[] = "envbaseline";
constexpr char BASELINE_NVS_KEY[] = "state";

AdaptiveBaselineConfig makeAdaptiveBaselineConfig()
{
    AdaptiveBaselineConfig config;
    // These are counted in SAMPLES, but what they mean is a DURATION. Deriving
    // them from SENSE_INTERVAL_MS keeps that meaning fixed: shortening the
    // sensing interval must make the screen more responsive, not make the
    // baseline learn from a fifth of the evidence or relearn five times sooner.
    constexpr unsigned long kWarmupMs = 4UL * 60UL * 1000UL;   // 4 min
    constexpr unsigned long kRelearnMs = 60UL * 60UL * 1000UL; // 1 h

    config.warmupSamples =
        static_cast<uint16_t>(kWarmupMs / SENSE_INTERVAL_MS);
    config.relearnAfterOutsideSamples =
        static_cast<uint32_t>(kRelearnMs / SENSE_INTERVAL_MS);

    // Write throttle: bounded by wall clock as well, so flash wear does not
    // scale with the sensing rate.
    config.persistEveryLearnedSamples =
        static_cast<uint16_t>(kRelearnMs / SENSE_INTERVAL_MS);
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
// 告警页上显示过的数值（放大十倍取整）。只在显示内容真会变化时重绘，
// 避免每秒无谓地全屏刷一次 I2C。
int lastAlarmTempTenths = INT32_MIN;
int lastAlarmHumTenths = INT32_MIN;
bool baselineWasReady = false;
bool invalidSampleAlert = false;
bool thresholdAlarmActive = false;
uint8_t alarmSafeSamples = 0;
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
unsigned long lastEdgeSampleMs = 0;
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

// OLED 字库把 '[' 复用成度数符号（见 oled.cpp 的 font5x7 表尾），
// 字库范围是 0x20..0x5B，所以下面用到的 % . 数字 大写字母都有字模。
constexpr char OLED_DEGREE = '[';

static void appendTemp(String &out, float value)
{
    out += String(value, 1);
    out += OLED_DEGREE;
    out += 'C';
}

/**
 * 告警原因文本。
 *
 * 只写"ENV ALARM"等于没说 —— 站在屏幕前的人需要知道是温度还是湿度、
 * 现在多少、门限多少，才能判断要不要处理。告警页有 5 行 × 21 字符
 * （drawWrapped，128/6），装得下实际数值。
 */
static String formatThresholdReason(float temperature, float humidity)
{
    String reason = "ALARM  ";
    bool first = true;

    if (temperature >= ALARM_TEMP_C)
    {
        reason += "TEMP ";
        appendTemp(reason, temperature);
        reason += " LIMIT ";
        appendTemp(reason, ALARM_TEMP_C);
        first = false;
    }
    if (humidity >= ALARM_HUMIDITY_PCT)
    {
        if (!first)
            reason += "  ";
        reason += "HUMIDITY ";
        reason += String(humidity, 1);
        reason += "% LIMIT ";
        reason += String(ALARM_HUMIDITY_PCT, 1);
        reason += '%';
    }
    return reason;
}

/// 硬限用不同的首词，免得和阈值告警在屏幕上分不清。
static String formatHardLimitReason(float temperature, float humidity)
{
    const AdaptiveBaselineConfig &config = adaptiveBaseline.config();
    String reason = "HARD LIMIT  ";

    if (temperature <= config.hardTempLowerC)
    {
        reason += "TEMP ";
        appendTemp(reason, temperature);
        reason += " MIN ";
        appendTemp(reason, config.hardTempLowerC);
    }
    else if (temperature >= config.hardTempUpperC)
    {
        reason += "TEMP ";
        appendTemp(reason, temperature);
        reason += " MAX ";
        appendTemp(reason, config.hardTempUpperC);
    }

    if (humidity <= config.hardHumidityLowerPct)
    {
        reason += "  HUMIDITY ";
        reason += String(humidity, 1);
        reason += "% MIN ";
        reason += String(config.hardHumidityLowerPct, 1);
        reason += '%';
    }
    else if (humidity >= config.hardHumidityUpperPct)
    {
        reason += "  HUMIDITY ";
        reason += String(humidity, 1);
        reason += "% MAX ";
        reason += String(config.hardHumidityUpperPct, 1);
        reason += '%';
    }
    return reason;
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
            OLED::showAlert(
                formatHardLimitReason(temperature, humidity));
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

/**
 * 本地阈值告警 —— 整条路径没有一次网络调用。
 *
 * 判定用的是安全任务 1 Hz 刷新的快照，所以检测周期是 1 秒，
 * 而不是 10 秒的上报周期：告警不该等一个上报窗口。
 *
 * 迟滞和硬限那套一致：越过立即响，要连续 ALARM_CLEAR_SAFE_SAMPLES 个
 * 安全样本才解除，避免值贴着阈值时来回叫。
 *
 * 样本无效（传感器掉线/读数不合法）时保持当前状态不变 —— 一次读失败
 * 不足以断定环境已经安全，静音一个可能真实的告警比误报更危险。
 */
static void applyThresholdAlarm()
{
    const SafetySnapshot snapshot = loadSafetySnapshot();
    if (!snapshot.valid)
        return;

    const bool breached =
        snapshot.temperature >= ALARM_TEMP_C ||
        snapshot.humidity >= ALARM_HUMIDITY_PCT;

    if (breached)
    {
        alarmSafeSamples = 0;

        if (!thresholdAlarmActive)
        {
            thresholdAlarmActive = true;
            HttpClient::setLocalAlarm(true);
            HttpClient::setBuzzer(true);
            // 打印屏幕上的原话，排查时不用凑到 OLED 前面看。
            // 串口里 '[' 就是屏幕上的度数符号。
            Serial.print("[ALARM] buzzer on, OLED: ");
            Serial.println(
                formatThresholdReason(snapshot.temperature,
                                      snapshot.humidity));
        }

        // 告警页独占屏幕，轮播不会把它换走 —— 但页上的数字必须跟着实测值走。
        // 只在跳变时写一次的话，屏幕会停在触发瞬间的快照上：手还捂着、温度还在
        // 涨，显示却纹丝不动，看上去像死机而不是像在监测。
        const int tempTenths = lroundf(snapshot.temperature * 10.0f);
        const int humTenths = lroundf(snapshot.humidity * 10.0f);
        if (tempTenths != lastAlarmTempTenths ||
            humTenths != lastAlarmHumTenths)
        {
            lastAlarmTempTenths = tempTenths;
            lastAlarmHumTenths = humTenths;
            OLED::showAlert(
                formatThresholdReason(snapshot.temperature,
                                      snapshot.humidity));
        }
        return;
    }

    if (!thresholdAlarmActive)
        return;

    if (alarmSafeSamples < ALARM_CLEAR_SAFE_SAMPLES)
    {
        ++alarmSafeSamples;
        return;
    }

    thresholdAlarmActive = false;
    alarmSafeSamples = 0;
    lastAlarmTempTenths = INT32_MIN;
    lastAlarmHumTenths = INT32_MIN;
    HttpClient::setBuzzer(false);
    HttpClient::setLocalAlarm(false);
    OLED::clearAlert();
    Serial.println("[ALARM] Cleared after stable safe samples");
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
        // 告警必须跟着采样跑，不能跟着 loop() 跑：loop() 要等 setup() 里的
        // WiFi/NTP/MQTT 全部结束，开机即超标的情况会被压后三十多秒才响。
        // 这个任务在联网之前就已启动，所以放这里才真正与网络无关。
        applyThresholdAlarm();
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

    // 本地告警期间云端命令同样不能按掉蜂鸣器，理由和本地命令一致。
    if (HttpClient::localAlarm())
    {
        Serial.println("[BUZZER] Cloud command ignored: local alarm active");
        publishCommandResponse(requestId, 1, "local_alarm_active");
        return;
    }

#if !ENABLE_BUZZER
    (void)enabled;
    Serial.println("[BUZZER] Cloud command rejected: hardware disabled");
    publishCommandResponse(requestId, 1, "hardware_disabled");
    return;
#else
    HttpClient::setBuzzer(enabled);
    Serial.print("[BUZZER] ");
    Serial.println(enabled ? "ON" : "OFF");

    const long durationMs = doc["paras"]["duration_ms"] | 0;
    if (enabled && durationMs > 0 && durationMs <= 30000)
    {
        delay(durationMs);
        HttpClient::setBuzzer(false);
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
    // 第一件事就把蜂鸣器引脚拉到静音电平。放在 Serial.begin + delay(3000)
    // 之后的话，开机前三秒 GPIO 是悬空的，低电平触发的模块在这段时间会一直响。
    HttpClient::initActuators();

    // 屏幕在烧录期间保持着上一版固件的最后一帧 —— 芯片在 bootloader 里，
    // 没人去改显存。所以新固件一起来就先清屏，别等 delay(3000) 之后，
    // 否则每次烧完都要盯着三秒钟的残留画面。
    const bool oledReady = OLED::init();

    // 自检紧跟引脚初始化，不等 Serial.begin 后面那 3 秒。它不需要串口，而放在
    // 延时之后会让"上电即自检"变成"上电三秒后自检" —— 对着板子看的人只会
    // 觉得它启动很慢。
#if ENABLE_BUZZER
    HttpClient::selfTestBuzzer();
#endif

    Serial.begin(115200);
    delay(3000);

    Serial.println();
    Serial.println("=== Edge IoT Monitor (Combined: local + cloud) ===");
    Serial.printf("[HW] Flash=%u bytes PSRAM=%u bytes\n",
                  ESP.getFlashChipSize(),
                  ESP.getPsramSize());

    // 把编译进来的阈值打出来。这些值分散在 config.h 和几处 #ifndef 兜底里，
    // 靠读源码推断哪个生效过一次错，就会在演示时对着一个自己以为的数字调试。
    Serial.printf("[CONFIG] alarm temp=%.1fC humidity=%.1f%% "
                  "buzzer=%s active=%s sense=%lums cloud=%lums\n",
                  ALARM_TEMP_C,
                  ALARM_HUMIDITY_PCT,
                  ENABLE_BUZZER ? "on" : "off",
                  BUZZER_ACTIVE_LEVEL == LOW ? "LOW" : "HIGH",
                  (unsigned long)SENSE_INTERVAL_MS,
                  (unsigned long)CLOUD_INTERVAL_MS);

    Serial.println(ENABLE_BUZZER
                       ? "[BUZZER] Self-test beep done at power-on"
                       : "[BUZZER] Disabled by safe firmware default");

    Serial.println(oledReady
                       ? "[OLED] Init OK; BOOT button advances pages"
                       : "[OLED] Init FAILED");

    initAdaptiveBaseline();

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
        applyThresholdAlarm();
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
            CLOUD_INTERVAL_MS)
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
    if (reportNow - lastReasoningReportMs < SENSE_INTERVAL_MS)
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

        // EdgeReasoner 的窗口是固定的 12 个样本，而它的阈值（极差 4C/15%RH、
        // 净升 0.8C）是按 2 分钟窗口整定的。若直接按感知周期喂，把感知周期从
        // 10 s 缩到 2 s 就等于把窗口缩到 24 s——阈值没变，含义却变了：净升那
        // 一条会取代速率成为实际门槛，"快升"需要的斜率反而被抬高。
        //
        // 所以推理按自己的固定节奏取样，与显示刷新率解耦。屏幕上的数字仍每
        // 2 s 更新，趋势判据仍在 2 分钟的证据上做出。
        if (reportNow - lastEdgeSampleMs >= EDGE_SAMPLE_INTERVAL_MS)
        {
            lastEdgeSampleMs = reportNow;
            edgeReasoner.add(temp, humi, millis());
        }
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
    if (latestBaseline.status == AdaptiveBaselineStatus::LEARNING &&
        baselineWasReady)
    {
        Serial.println("[BASELINE] Sustained departure — relearning this "
                       "environment from scratch");
    }
    baselineWasReady = latestBaseline.ready;

    Serial.print("[BASELINE] status=");
    Serial.print(AdaptiveBaseline::statusName(latestBaseline.status));
    Serial.print(" progress=");
    Serial.print(latestBaseline.progressPct);
    Serial.print("% samples=");
    Serial.println(latestBaseline.learnedSamples);

    OLED::updateDashboard(
        temp, humi, assessment, latestBaseline, mqtt.connected());

    // One combined MQTT message carries both services per cloud slot. This is
    // the only metered path, so it is throttled separately from reasoning.
    if (!loadPendingHardLimitEvent().pending &&
        mqtt.connected() &&
        reportNow - lastCloudReportMs >= CLOUD_INTERVAL_MS &&
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

    // 本地服务是验证夹具，不是数据链路的一环 —— 判决在芯片上，记录在 IoTDA。
    // 它掉线时设备的行为一个字节都不变，所以不该抢占屏幕报一屏 "OFFLINE"：
    // 那会把"可选的落库目标不在"呈现成"设备出故障了"。
    // WiFi 与 MQTT 的状态仍然上屏，因为那条是云端记录的通路。
    const bool backendNow = HttpClient::backendReachable();
    if (backendNow != backendWasReachable)
    {
        backendWasReachable = backendNow;
        Serial.println(backendNow
                           ? "[BACKEND] Local recorder reachable"
                           : "[BACKEND] Local recorder unreachable — "
                             "optional sink, device unaffected");
    }

    if (backendNow)
        HttpClient::pollAndApplyCommand();
}
