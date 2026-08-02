#include "edge_reasoner.h"

#include <math.h>

namespace
{
constexpr size_t MIN_BASELINE_SAMPLES = 4;
constexpr float HIGH_TEMP_C = 30.0f;
constexpr float HIGH_HUMIDITY_PCT = 70.0f;
constexpr float RAPID_TEMP_C_PER_MIN = 1.2f;
constexpr float RAPID_HUMIDITY_PCT_PER_MIN = 4.0f;
constexpr float UNSTABLE_TEMP_RANGE_C = 4.0f;
constexpr float UNSTABLE_HUMIDITY_RANGE_PCT = 15.0f;
constexpr float UNSTABLE_TEMP_STEP_C = 2.5f;
constexpr float UNSTABLE_HUMIDITY_STEP_PCT = 10.0f;
}

void EdgeReasoner::add(float temperature, float humidity, unsigned long capturedAtMs)
{
    samples_[next_].temperature = temperature;
    samples_[next_].humidity = humidity;
    samples_[next_].capturedAtMs = capturedAtMs;
    next_ = (next_ + 1) % WINDOW_SIZE;
    if (count_ < WINDOW_SIZE)
        ++count_;
}

size_t EdgeReasoner::sampleCount() const
{
    return count_;
}

const EdgeReasoner::Sample &EdgeReasoner::chronological(size_t index) const
{
    const size_t oldest = count_ == WINDOW_SIZE ? next_ : 0;
    return samples_[(oldest + index) % WINDOW_SIZE];
}

float EdgeReasoner::clampConfidence(float value)
{
    if (value < 0.0f)
        return 0.0f;
    if (value > 0.99f)
        return 0.99f;
    return value;
}

EdgeAssessment EdgeReasoner::assess() const
{
    if (count_ < MIN_BASELINE_SAMPLES)
    {
        const float progress = static_cast<float>(count_) /
                               static_cast<float>(MIN_BASELINE_SAMPLES);
        return {"WARMUP", "info", 0.35f + progress * 0.25f,
                "LEARNING_BASELINE", "Collecting a local trend baseline.",
                "LEARNING"};
    }

    const Sample &first = chronological(0);
    const Sample &last = chronological(count_ - 1);
    float tempMin = first.temperature;
    float tempMax = first.temperature;
    float humMin = first.humidity;
    float humMax = first.humidity;
    float maxTempStep = 0.0f;
    float maxHumStep = 0.0f;

    for (size_t i = 1; i < count_; ++i)
    {
        const Sample &previous = chronological(i - 1);
        const Sample &current = chronological(i);
        tempMin = fminf(tempMin, current.temperature);
        tempMax = fmaxf(tempMax, current.temperature);
        humMin = fminf(humMin, current.humidity);
        humMax = fmaxf(humMax, current.humidity);
        maxTempStep = fmaxf(maxTempStep,
                            fabsf(current.temperature - previous.temperature));
        maxHumStep = fmaxf(maxHumStep,
                           fabsf(current.humidity - previous.humidity));
    }

    const float tempRange = tempMax - tempMin;
    const float humRange = humMax - humMin;
    const unsigned long elapsedMs = last.capturedAtMs - first.capturedAtMs;
    const float elapsedMinutes = elapsedMs > 0
        ? static_cast<float>(elapsedMs) / 60000.0f
        : 0.0f;
    const float tempSlope = elapsedMinutes > 0.0f
        ? (last.temperature - first.temperature) / elapsedMinutes
        : 0.0f;
    const float humSlope = elapsedMinutes > 0.0f
        ? (last.humidity - first.humidity) / elapsedMinutes
        : 0.0f;

    if (tempRange >= UNSTABLE_TEMP_RANGE_C ||
        humRange >= UNSTABLE_HUMIDITY_RANGE_PCT ||
        maxTempStep >= UNSTABLE_TEMP_STEP_C ||
        maxHumStep >= UNSTABLE_HUMIDITY_STEP_PCT)
    {
        const float margin = fmaxf(tempRange / UNSTABLE_TEMP_RANGE_C,
                                   humRange / UNSTABLE_HUMIDITY_RANGE_PCT);
        return {"UNSTABLE", "warning", clampConfidence(0.62f + margin * 0.12f),
                "ERRATIC_SIGNAL", "Readings are changing too sharply to trust.",
                "SENSOR UNSTABLE"};
    }

    if (last.temperature >= HIGH_TEMP_C &&
        last.humidity >= HIGH_HUMIDITY_PCT)
    {
        const float margin = fmaxf((last.temperature - HIGH_TEMP_C) / 8.0f,
                                   (last.humidity - HIGH_HUMIDITY_PCT) / 25.0f);
        return {"HEAT_HUMID_RISK", "warning",
                clampConfidence(0.72f + margin * 0.20f),
                "HOT_AND_HUMID", "Temperature and humidity are both elevated.",
                "HOT + HUMID"};
    }

    if (tempSlope >= RAPID_TEMP_C_PER_MIN &&
        last.temperature - first.temperature >= 0.8f)
    {
        return {"TEMP_RISING", "warning",
                clampConfidence(0.65f + tempSlope / 12.0f),
                "RAPID_TEMP_RISE", "Temperature is rising rapidly.",
                "TEMP RISING"};
    }

    if (humSlope >= RAPID_HUMIDITY_PCT_PER_MIN &&
        last.humidity - first.humidity >= 3.0f)
    {
        return {"HUMIDITY_RISING", "warning",
                clampConfidence(0.65f + humSlope / 30.0f),
                "RAPID_HUMIDITY_RISE", "Humidity is rising rapidly.",
                "HUMI RISING"};
    }

    if (last.temperature >= HIGH_TEMP_C)
    {
        return {"HIGH_TEMPERATURE", "watch",
                clampConfidence(0.68f + (last.temperature - HIGH_TEMP_C) / 15.0f),
                "TEMP_ABOVE_COMFORT", "Temperature is above the comfort threshold.",
                "HIGH TEMP"};
    }

    if (last.humidity >= HIGH_HUMIDITY_PCT)
    {
        return {"HIGH_HUMIDITY", "watch",
                clampConfidence(0.68f + (last.humidity - HIGH_HUMIDITY_PCT) / 35.0f),
                "HUMIDITY_ABOVE_COMFORT", "Humidity is above the comfort threshold.",
                "HIGH HUMIDITY"};
    }

    const float stability = 1.0f -
        fminf(1.0f, tempRange / UNSTABLE_TEMP_RANGE_C * 0.5f +
                    humRange / UNSTABLE_HUMIDITY_RANGE_PCT * 0.5f);
    return {"NORMAL", "info", clampConfidence(0.70f + stability * 0.25f),
            "STABLE_ENVIRONMENT", "Temperature and humidity are stable.",
            "NORMAL"};
}
