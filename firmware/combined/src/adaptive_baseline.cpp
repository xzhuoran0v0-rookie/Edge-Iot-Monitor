#include "adaptive_baseline.h"

#include <math.h>
#include <string.h>

namespace
{
constexpr uint32_t PERSISTENT_MAGIC = 0x4E4C4241UL; // "ABLN" little-endian
constexpr uint16_t PERSISTENT_VERSION = 2;
constexpr uint32_t FNV_OFFSET_BASIS = 2166136261UL;
constexpr uint32_t FNV_PRIME = 16777619UL;

template <typename T>
void hashValue(uint32_t &hash, const T &value)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
}

float clampFloat(float value, float lower, float upper)
{
    if (value < lower)
        return lower;
    if (value > upper)
        return upper;
    return value;
}

bool finiteFloat(float value)
{
    return isfinite(value);
}

bool validRange(float lower, float upper)
{
    return finiteFloat(lower) && finiteFloat(upper) && lower < upper;
}

uint32_t saturatingIncrement(uint32_t value)
{
    return value == UINT32_MAX ? value : value + 1U;
}
}

static_assert(sizeof(float) == 4,
              "AdaptiveBaseline persistence requires 32-bit float");
static_assert(offsetof(AdaptiveBaselinePersistentState, checksum) == 36,
              "Unexpected persistent-state layout");
static_assert(sizeof(AdaptiveBaselinePersistentState) == 40,
              "Unexpected persistent-state size");

AdaptiveBaseline::AdaptiveBaseline()
    : AdaptiveBaseline(AdaptiveBaselineConfig())
{
}

AdaptiveBaseline::AdaptiveBaseline(const AdaptiveBaselineConfig &config)
    : config_(sanitizeConfig(config)),
      configSignature_(calculateConfigSignature(config_))
{
    clearRuntimeState(false);
}

AdaptiveBaselineConfig AdaptiveBaseline::sanitizeConfig(
    const AdaptiveBaselineConfig &input)
{
    const AdaptiveBaselineConfig defaults;
    AdaptiveBaselineConfig value = input;

    // A warmup shorter than a few seconds cannot characterise a room, and one
    // longer than a day means the baseline never becomes usable.
    if (value.warmupMs < 5UL * 1000UL ||
        value.warmupMs > 24UL * 60UL * 60UL * 1000UL)
    {
        value.warmupMs = defaults.warmupMs;
    }
    if (value.persistEveryLearnedSamples == 0)
        value.persistEveryLearnedSamples =
            defaults.persistEveryLearnedSamples;

    if (!validRange(value.physicalTempLowerC,
                    value.physicalTempUpperC))
    {
        value.physicalTempLowerC = defaults.physicalTempLowerC;
        value.physicalTempUpperC = defaults.physicalTempUpperC;
    }
    if (!validRange(value.physicalHumidityLowerPct,
                    value.physicalHumidityUpperPct))
    {
        value.physicalHumidityLowerPct =
            defaults.physicalHumidityLowerPct;
        value.physicalHumidityUpperPct =
            defaults.physicalHumidityUpperPct;
    }

    if (!validRange(value.hardTempLowerC, value.hardTempUpperC) ||
        value.hardTempLowerC < value.physicalTempLowerC ||
        value.hardTempUpperC > value.physicalTempUpperC)
    {
        if (defaults.hardTempLowerC >= value.physicalTempLowerC &&
            defaults.hardTempUpperC <= value.physicalTempUpperC)
        {
            value.hardTempLowerC = defaults.hardTempLowerC;
            value.hardTempUpperC = defaults.hardTempUpperC;
        }
        else
        {
            const float margin =
                (value.physicalTempUpperC -
                 value.physicalTempLowerC) * 0.1f;
            value.hardTempLowerC =
                value.physicalTempLowerC + margin;
            value.hardTempUpperC =
                value.physicalTempUpperC - margin;
        }
    }
    if (!validRange(value.hardHumidityLowerPct,
                    value.hardHumidityUpperPct) ||
        value.hardHumidityLowerPct < value.physicalHumidityLowerPct ||
        value.hardHumidityUpperPct > value.physicalHumidityUpperPct)
    {
        if (defaults.hardHumidityLowerPct >=
                value.physicalHumidityLowerPct &&
            defaults.hardHumidityUpperPct <=
                value.physicalHumidityUpperPct)
        {
            value.hardHumidityLowerPct =
                defaults.hardHumidityLowerPct;
            value.hardHumidityUpperPct =
                defaults.hardHumidityUpperPct;
        }
        else
        {
            const float margin =
                (value.physicalHumidityUpperPct -
                 value.physicalHumidityLowerPct) * 0.1f;
            value.hardHumidityLowerPct =
                value.physicalHumidityLowerPct + margin;
            value.hardHumidityUpperPct =
                value.physicalHumidityUpperPct - margin;
        }
    }

    const float tempHardSpan =
        value.hardTempUpperC - value.hardTempLowerC;
    const float humidityHardSpan =
        value.hardHumidityUpperPct - value.hardHumidityLowerPct;

    const float maxTempGuard = tempHardSpan * 0.49f;
    const float maxHumidityGuard = humidityHardSpan * 0.49f;
    if (!finiteFloat(value.tempHardGuardC) ||
        value.tempHardGuardC < 0.0f ||
        value.tempHardGuardC > maxTempGuard)
    {
        value.tempHardGuardC =
            fminf(defaults.tempHardGuardC, tempHardSpan * 0.1f);
    }
    if (!finiteFloat(value.humidityHardGuardPct) ||
        value.humidityHardGuardPct < 0.0f ||
        value.humidityHardGuardPct > maxHumidityGuard)
    {
        value.humidityHardGuardPct =
            fminf(defaults.humidityHardGuardPct,
                  humidityHardSpan * 0.1f);
    }

    const float maxTempHalfBand =
        (tempHardSpan - 2.0f * value.tempHardGuardC) * 0.5f;
    const float maxHumidityHalfBand =
        (humidityHardSpan -
         2.0f * value.humidityHardGuardPct) * 0.5f;

    if (!finiteFloat(value.minTempHalfBandC) ||
        value.minTempHalfBandC <= 0.0f)
    {
        value.minTempHalfBandC = defaults.minTempHalfBandC;
    }
    if (!finiteFloat(value.maxTempHalfBandC) ||
        value.maxTempHalfBandC < value.minTempHalfBandC)
    {
        value.maxTempHalfBandC =
            value.minTempHalfBandC > defaults.maxTempHalfBandC
                ? value.minTempHalfBandC
                : defaults.maxTempHalfBandC;
    }
    value.maxTempHalfBandC =
        clampFloat(value.maxTempHalfBandC,
                   value.minTempHalfBandC,
                   maxTempHalfBand);
    value.minTempHalfBandC =
        clampFloat(value.minTempHalfBandC,
                   0.01f,
                   value.maxTempHalfBandC);

    if (!finiteFloat(value.minHumidityHalfBandPct) ||
        value.minHumidityHalfBandPct <= 0.0f)
    {
        value.minHumidityHalfBandPct =
            defaults.minHumidityHalfBandPct;
    }
    if (!finiteFloat(value.maxHumidityHalfBandPct) ||
        value.maxHumidityHalfBandPct <
            value.minHumidityHalfBandPct)
    {
        value.maxHumidityHalfBandPct =
            value.minHumidityHalfBandPct >
                    defaults.maxHumidityHalfBandPct
                ? value.minHumidityHalfBandPct
                : defaults.maxHumidityHalfBandPct;
    }
    value.maxHumidityHalfBandPct =
        clampFloat(value.maxHumidityHalfBandPct,
                   value.minHumidityHalfBandPct,
                   maxHumidityHalfBand);
    value.minHumidityHalfBandPct =
        clampFloat(value.minHumidityHalfBandPct,
                   0.01f,
                   value.maxHumidityHalfBandPct);

    if (!finiteFloat(value.deviationMultiplier) ||
        value.deviationMultiplier < 1.0f ||
        value.deviationMultiplier > 8.0f)
    {
        value.deviationMultiplier = defaults.deviationMultiplier;
    }
    if (!finiteFloat(value.centerLearningRate) ||
        value.centerLearningRate <= 0.0f ||
        value.centerLearningRate > 0.25f)
    {
        value.centerLearningRate = defaults.centerLearningRate;
    }
    if (!finiteFloat(value.deviationLearningRate) ||
        value.deviationLearningRate <= 0.0f ||
        value.deviationLearningRate > 0.25f)
    {
        value.deviationLearningRate =
            defaults.deviationLearningRate;
    }

    if (!finiteFloat(value.warmupTempDeltaClampC) ||
        value.warmupTempDeltaClampC <= 0.0f)
    {
        value.warmupTempDeltaClampC =
            defaults.warmupTempDeltaClampC;
    }
    if (!finiteFloat(value.warmupHumidityDeltaClampPct) ||
        value.warmupHumidityDeltaClampPct <= 0.0f)
    {
        value.warmupHumidityDeltaClampPct =
            defaults.warmupHumidityDeltaClampPct;
    }

    return value;
}

uint32_t AdaptiveBaseline::calculateConfigSignature(
    const AdaptiveBaselineConfig &config)
{
    uint32_t hash = FNV_OFFSET_BASIS;
    hashValue(hash, config.warmupMs);
    hashValue(hash, config.physicalTempLowerC);
    hashValue(hash, config.physicalTempUpperC);
    hashValue(hash, config.physicalHumidityLowerPct);
    hashValue(hash, config.physicalHumidityUpperPct);
    hashValue(hash, config.hardTempLowerC);
    hashValue(hash, config.hardTempUpperC);
    hashValue(hash, config.hardHumidityLowerPct);
    hashValue(hash, config.hardHumidityUpperPct);
    hashValue(hash, config.tempHardGuardC);
    hashValue(hash, config.humidityHardGuardPct);
    hashValue(hash, config.minTempHalfBandC);
    hashValue(hash, config.maxTempHalfBandC);
    hashValue(hash, config.minHumidityHalfBandPct);
    hashValue(hash, config.maxHumidityHalfBandPct);
    hashValue(hash, config.deviationMultiplier);
    hashValue(hash, config.centerLearningRate);
    hashValue(hash, config.deviationLearningRate);
    hashValue(hash, config.warmupTempDeltaClampC);
    hashValue(hash, config.warmupHumidityDeltaClampPct);
    // Deliberately excluded: persistEveryLearnedSamples, minPersistIntervalMs
    // and relearnAfterOutsideMs. They are policy, not semantics — they do
    // not change what a stored center/deviation means, so changing one must not
    // throw away an otherwise valid baseline.
    return hash;
}

uint32_t AdaptiveBaseline::calculateStateChecksum(
    const AdaptiveBaselinePersistentState &state)
{
    uint32_t hash = FNV_OFFSET_BASIS;
    hashValue(hash, state.magic);
    hashValue(hash, state.version);
    hashValue(hash, state.size);
    hashValue(hash, state.configSignature);
    hashValue(hash, state.learnedSamples);
    hashValue(hash, state.warmupElapsedMs);
    hashValue(hash, state.temperatureCenterC);
    hashValue(hash, state.humidityCenterPct);
    hashValue(hash, state.temperatureDeviationC);
    hashValue(hash, state.humidityDeviationPct);
    return hash;
}

void AdaptiveBaseline::clearRuntimeState(bool persistenceRequired)
{
    warmupElapsedMs_ = 0;
    lastObserveMs_ = 0;
    haveLastObserve_ = false;
    firstOutsideMs_ = 0;
    outsidePending_ = false;
    learnedSamples_ = 0;
    temperatureCenterC_ = 0.0f;
    humidityCenterPct_ = 0.0f;
    temperatureDeviationC_ = 0.0f;
    humidityDeviationPct_ = 0.0f;
    lastStatus_ = AdaptiveBaselineStatus::RESET;
    seedPending_ = false;
    seedTemperatureC_ = 0.0f;
    seedHumidityPct_ = 0.0f;
    learnedSincePersist_ = 0;
    lastPersistMs_ = 0;
    persistedOnce_ = false;
    dirty_ = persistenceRequired;
    forcePersistence_ = persistenceRequired;
}

void AdaptiveBaseline::reset()
{
    clearRuntimeState(true);
}

bool AdaptiveBaseline::ready() const
{
    return warmupElapsedMs_ >= config_.warmupMs;
}

uint8_t AdaptiveBaseline::progressPercent() const
{
    if (ready())
        return 100;

    const uint32_t progress =
        static_cast<uint32_t>(
            static_cast<uint64_t>(warmupElapsedMs_) * 100ULL /
            config_.warmupMs);
    return static_cast<uint8_t>(progress > 100UL ? 100UL : progress);
}

uint32_t AdaptiveBaseline::learnedSampleCount() const
{
    return learnedSamples_;
}

const AdaptiveBaselineConfig &AdaptiveBaseline::config() const
{
    return config_;
}

void AdaptiveBaseline::calculateBands(
    float &temperatureLowerC,
    float &temperatureUpperC,
    float &humidityLowerPct,
    float &humidityUpperPct) const
{
    if (learnedSamples_ == 0)
    {
        temperatureLowerC =
            config_.hardTempLowerC + config_.tempHardGuardC;
        temperatureUpperC =
            config_.hardTempUpperC - config_.tempHardGuardC;
        humidityLowerPct =
            config_.hardHumidityLowerPct +
            config_.humidityHardGuardPct;
        humidityUpperPct =
            config_.hardHumidityUpperPct -
            config_.humidityHardGuardPct;
        return;
    }

    const float tempHalfWidth = clampFloat(
        temperatureDeviationC_ * config_.deviationMultiplier,
        config_.minTempHalfBandC,
        config_.maxTempHalfBandC);
    const float humidityHalfWidth = clampFloat(
        humidityDeviationPct_ * config_.deviationMultiplier,
        config_.minHumidityHalfBandPct,
        config_.maxHumidityHalfBandPct);

    const float innerTempLower =
        config_.hardTempLowerC + config_.tempHardGuardC;
    const float innerTempUpper =
        config_.hardTempUpperC - config_.tempHardGuardC;
    const float innerHumidityLower =
        config_.hardHumidityLowerPct +
        config_.humidityHardGuardPct;
    const float innerHumidityUpper =
        config_.hardHumidityUpperPct -
        config_.humidityHardGuardPct;
    const float boundedTempCenter = clampFloat(
        temperatureCenterC_, innerTempLower, innerTempUpper);
    const float boundedHumidityCenter = clampFloat(
        humidityCenterPct_, innerHumidityLower, innerHumidityUpper);

    temperatureLowerC = fmaxf(
        boundedTempCenter - tempHalfWidth, innerTempLower);
    temperatureUpperC = fminf(
        boundedTempCenter + tempHalfWidth, innerTempUpper);
    humidityLowerPct = fmaxf(
        boundedHumidityCenter - humidityHalfWidth,
        innerHumidityLower);
    humidityUpperPct = fminf(
        boundedHumidityCenter + humidityHalfWidth,
        innerHumidityUpper);
}

AdaptiveBaselineResult AdaptiveBaseline::makeResult(
    AdaptiveBaselineStatus status) const
{
    AdaptiveBaselineResult result;
    result.status = status;
    result.ready = ready();
    result.progressPct = progressPercent();
    result.learnedSamples = learnedSamples_;
    result.temperatureDeviationC = temperatureDeviationC_;
    result.humidityDeviationPct = humidityDeviationPct_;
    result.temperatureCenterC = temperatureCenterC_;
    result.humidityCenterPct = humidityCenterPct_;
    calculateBands(result.temperatureLowerC,
                   result.temperatureUpperC,
                   result.humidityLowerPct,
                   result.humidityUpperPct);
    return result;
}

AdaptiveBaselineResult AdaptiveBaseline::snapshot() const
{
    return makeResult(lastStatus_);
}

void AdaptiveBaseline::learnWarmup(float temperatureC,
                                   float humidityPct)
{
    if (learnedSamples_ == 0)
    {
        temperatureCenterC_ = temperatureC;
        humidityCenterPct_ = humidityPct;
        temperatureDeviationC_ = 0.0f;
        humidityDeviationPct_ = 0.0f;
    }
    else
    {
        const float tempDelta = clampFloat(
            temperatureC - temperatureCenterC_,
            -config_.warmupTempDeltaClampC,
            config_.warmupTempDeltaClampC);
        const float humidityDelta = clampFloat(
            humidityPct - humidityCenterPct_,
            -config_.warmupHumidityDeltaClampPct,
            config_.warmupHumidityDeltaClampPct);
        const uint32_t nextCount = saturatingIncrement(learnedSamples_);
        // A plain running mean over the warmup, but never slower than the
        // steady-state rate — warmup is now a duration, so there is no sample
        // count to cap the denominator with.
        const float alpha = fmaxf(
            1.0f / static_cast<float>(nextCount),
            config_.centerLearningRate);

        const float boundedTemp =
            temperatureCenterC_ + tempDelta;
        const float boundedHumidity =
            humidityCenterPct_ + humidityDelta;
        const float tempAbsoluteDeviation =
            fabsf(boundedTemp - temperatureCenterC_);
        const float humidityAbsoluteDeviation =
            fabsf(boundedHumidity - humidityCenterPct_);

        temperatureCenterC_ +=
            alpha * (boundedTemp - temperatureCenterC_);
        humidityCenterPct_ +=
            alpha * (boundedHumidity - humidityCenterPct_);
        temperatureDeviationC_ +=
            alpha *
            (tempAbsoluteDeviation - temperatureDeviationC_);
        humidityDeviationPct_ +=
            alpha *
            (humidityAbsoluteDeviation - humidityDeviationPct_);
    }

    learnedSamples_ = saturatingIncrement(learnedSamples_);
    learnedSincePersist_ =
        saturatingIncrement(learnedSincePersist_);
    dirty_ = true;
}

void AdaptiveBaseline::learnReady(float temperatureC,
                                  float humidityPct)
{
    const float tempAbsoluteDeviation =
        fabsf(temperatureC - temperatureCenterC_);
    const float humidityAbsoluteDeviation =
        fabsf(humidityPct - humidityCenterPct_);

    temperatureCenterC_ += config_.centerLearningRate *
        (temperatureC - temperatureCenterC_);
    humidityCenterPct_ += config_.centerLearningRate *
        (humidityPct - humidityCenterPct_);
    temperatureDeviationC_ += config_.deviationLearningRate *
        (tempAbsoluteDeviation - temperatureDeviationC_);
    humidityDeviationPct_ += config_.deviationLearningRate *
        (humidityAbsoluteDeviation - humidityDeviationPct_);

    learnedSamples_ = saturatingIncrement(learnedSamples_);
    learnedSincePersist_ =
        saturatingIncrement(learnedSincePersist_);
    dirty_ = true;
}

uint32_t AdaptiveBaseline::consumeElapsed(uint32_t nowMs)
{
    if (!haveLastObserve_)
    {
        haveLastObserve_ = true;
        lastObserveMs_ = nowMs;
        return 0;
    }

    // Unsigned subtraction, so a millis() wraparound still yields the real gap.
    uint32_t delta = nowMs - lastObserveMs_;
    lastObserveMs_ = nowMs;
    if (delta > kMaxObserveDeltaMs)
        delta = kMaxObserveDeltaMs;
    return delta;
}

AdaptiveBaselineResult AdaptiveBaseline::observe(
    float temperatureC,
    float humidityPct,
    uint32_t nowMs)
{
    // Charged once per call, before any early return: a sample that is refused
    // still tells us how much time passed, and the departure clock below has to
    // keep running while the caller is sampling at its slowest.
    const uint32_t elapsedMs = consumeElapsed(nowMs);
    const bool valid =
        finiteFloat(temperatureC) &&
        finiteFloat(humidityPct) &&
        temperatureC >= config_.physicalTempLowerC &&
        temperatureC <= config_.physicalTempUpperC &&
        humidityPct >= config_.physicalHumidityLowerPct &&
        humidityPct <= config_.physicalHumidityUpperPct;

    if (!valid)
    {
        lastStatus_ = AdaptiveBaselineStatus::INVALID_SAMPLE;
        AdaptiveBaselineResult result = makeResult(lastStatus_);
        result.validSample = false;
        return result;
    }

    const bool tempHard =
        temperatureC <= config_.hardTempLowerC ||
        temperatureC >= config_.hardTempUpperC;
    const bool humidityHard =
        humidityPct <= config_.hardHumidityLowerPct ||
        humidityPct >= config_.hardHumidityUpperPct;
    if (tempHard || humidityHard)
    {
        lastStatus_ = AdaptiveBaselineStatus::HARD_LIMIT;
        AdaptiveBaselineResult result = makeResult(lastStatus_);
        result.validSample = true;
        result.hardLimitExceeded = true;
        result.temperatureHardLimit = tempHard;
        result.humidityHardLimit = humidityHard;
        return result;
    }

    if (!ready())
    {
        // Do not trust the very first otherwise-valid reading as the center.
        // Require a second nearby sample. If it disagrees sharply, replace
        // the candidate and wait once more; this lets the baseline recover
        // automatically from a bad power-on reading.
        if (learnedSamples_ == 0)
        {
            const bool confirmsSeed =
                seedPending_ &&
                fabsf(temperatureC - seedTemperatureC_) <=
                    config_.warmupTempDeltaClampC &&
                fabsf(humidityPct - seedHumidityPct_) <=
                    config_.warmupHumidityDeltaClampPct;

            if (!confirmsSeed)
            {
                seedPending_ = true;
                seedTemperatureC_ = temperatureC;
                seedHumidityPct_ = humidityPct;
                lastStatus_ = AdaptiveBaselineStatus::LEARNING;
                AdaptiveBaselineResult result = makeResult(lastStatus_);
                result.validSample = true;
                return result;
            }

            temperatureCenterC_ =
                (seedTemperatureC_ + temperatureC) * 0.5f;
            humidityCenterPct_ =
                (seedHumidityPct_ + humidityPct) * 0.5f;
            temperatureDeviationC_ =
                fabsf(temperatureC - seedTemperatureC_) * 0.5f;
            humidityDeviationPct_ =
                fabsf(humidityPct - seedHumidityPct_) * 0.5f;
            learnedSamples_ = 2;
            learnedSincePersist_ = 2;
            warmupElapsedMs_ += elapsedMs;
            dirty_ = true;
            seedPending_ = false;

            lastStatus_ = ready()
                ? AdaptiveBaselineStatus::READY
                : AdaptiveBaselineStatus::LEARNING;
            AdaptiveBaselineResult result = makeResult(lastStatus_);
            result.validSample = true;
            result.learned = true;
            return result;
        }

        learnWarmup(temperatureC, humidityPct);
        warmupElapsedMs_ += elapsedMs;
        lastStatus_ = ready()
            ? AdaptiveBaselineStatus::READY
            : AdaptiveBaselineStatus::LEARNING;
        AdaptiveBaselineResult result = makeResult(lastStatus_);
        result.validSample = true;
        result.learned = true;
        return result;
    }

    float tempLower = 0.0f;
    float tempUpper = 0.0f;
    float humidityLower = 0.0f;
    float humidityUpper = 0.0f;
    calculateBands(tempLower, tempUpper,
                   humidityLower, humidityUpper);

    const bool tempOutside =
        temperatureC < tempLower || temperatureC > tempUpper;
    const bool humidityOutside =
        humidityPct < humidityLower || humidityPct > humidityUpper;
    if (tempOutside || humidityOutside)
    {
        if (!outsidePending_)
        {
            outsidePending_ = true;
            firstOutsideMs_ = nowMs;
        }

        // A sustained departure means the learned band describes somewhere
        // else. Without this the band can never move again: out-of-band
        // samples are not learned, so a relocated device stays in
        // BASELINE_SHIFT forever and nothing is ever written back to NVS.
        if (config_.relearnAfterOutsideMs > 0 &&
            nowMs - firstOutsideMs_ >= config_.relearnAfterOutsideMs)
        {
            reset();
            lastStatus_ = AdaptiveBaselineStatus::LEARNING;
            AdaptiveBaselineResult result = makeResult(lastStatus_);
            result.validSample = true;
            return result;
        }

        lastStatus_ =
            AdaptiveBaselineStatus::OUTSIDE_ADAPTIVE_BAND;
        AdaptiveBaselineResult result = makeResult(lastStatus_);
        result.validSample = true;
        result.outsideAdaptiveBand = true;
        result.temperatureOutsideBand = tempOutside;
        result.humidityOutsideBand = humidityOutside;
        return result;
    }

    // Back inside the band: the departure was transient, not a relocation.
    outsidePending_ = false;
    learnReady(temperatureC, humidityPct);
    lastStatus_ = AdaptiveBaselineStatus::READY;
    AdaptiveBaselineResult result = makeResult(lastStatus_);
    result.validSample = true;
    result.learned = true;
    return result;
}

AdaptiveBaselinePersistentState AdaptiveBaseline::exportState() const
{
    AdaptiveBaselinePersistentState state;
    state.magic = PERSISTENT_MAGIC;
    state.version = PERSISTENT_VERSION;
    state.size =
        static_cast<uint16_t>(sizeof(AdaptiveBaselinePersistentState));
    state.configSignature = configSignature_;
    state.learnedSamples = learnedSamples_;
    state.warmupElapsedMs = warmupElapsedMs_;
    state.temperatureCenterC = temperatureCenterC_;
    state.humidityCenterPct = humidityCenterPct_;
    state.temperatureDeviationC = temperatureDeviationC_;
    state.humidityDeviationPct = humidityDeviationPct_;
    state.checksum = calculateStateChecksum(state);
    return state;
}

bool AdaptiveBaseline::restoreState(
    const AdaptiveBaselinePersistentState &state,
    uint32_t nowMs)
{
    if (state.magic != PERSISTENT_MAGIC ||
        state.version != PERSISTENT_VERSION ||
        state.size != sizeof(AdaptiveBaselinePersistentState) ||
        state.configSignature != configSignature_ ||
        state.checksum != calculateStateChecksum(state))
    {
        return false;
    }

    if (!finiteFloat(state.temperatureCenterC) ||
        !finiteFloat(state.humidityCenterPct) ||
        !finiteFloat(state.temperatureDeviationC) ||
        !finiteFloat(state.humidityDeviationPct) ||
        state.temperatureDeviationC < 0.0f ||
        state.humidityDeviationPct < 0.0f)
    {
        return false;
    }

    if (state.learnedSamples > 0 &&
        (state.temperatureCenterC <= config_.hardTempLowerC ||
         state.temperatureCenterC >= config_.hardTempUpperC ||
         state.humidityCenterPct <= config_.hardHumidityLowerPct ||
         state.humidityCenterPct >= config_.hardHumidityUpperPct ||
         state.temperatureDeviationC >
             config_.hardTempUpperC - config_.hardTempLowerC ||
         state.humidityDeviationPct >
             config_.hardHumidityUpperPct -
                 config_.hardHumidityLowerPct))
    {
        return false;
    }

    learnedSamples_ = state.learnedSamples;
    warmupElapsedMs_ = state.warmupElapsedMs;
    haveLastObserve_ = false;
    firstOutsideMs_ = 0;
    outsidePending_ = false;
    temperatureCenterC_ = state.temperatureCenterC;
    humidityCenterPct_ = state.humidityCenterPct;
    temperatureDeviationC_ = state.temperatureDeviationC;
    humidityDeviationPct_ = state.humidityDeviationPct;
    seedPending_ = false;
    seedTemperatureC_ = 0.0f;
    seedHumidityPct_ = 0.0f;
    lastStatus_ = learnedSamples_ == 0
        ? AdaptiveBaselineStatus::RESET
        : (ready()
               ? AdaptiveBaselineStatus::READY
               : AdaptiveBaselineStatus::LEARNING);
    learnedSincePersist_ = 0;
    lastPersistMs_ = nowMs;
    persistedOnce_ = true;
    dirty_ = false;
    forcePersistence_ = false;
    return true;
}

bool AdaptiveBaseline::persistenceDue(uint32_t nowMs) const
{
    if (!dirty_)
        return false;
    if (forcePersistence_)
        return true;
    if (!ready())
        return false;
    if (!persistedOnce_)
        return true;
    if (learnedSincePersist_ <
        config_.persistEveryLearnedSamples)
    {
        return false;
    }
    return nowMs - lastPersistMs_ >=
        config_.minPersistIntervalMs;
}

void AdaptiveBaseline::markPersisted(uint32_t nowMs)
{
    learnedSincePersist_ = 0;
    lastPersistMs_ = nowMs;
    persistedOnce_ = true;
    dirty_ = false;
    forcePersistence_ = false;
}

bool AdaptiveBaseline::dirty() const
{
    return dirty_;
}

const char *AdaptiveBaseline::statusName(
    AdaptiveBaselineStatus status)
{
    switch (status)
    {
    case AdaptiveBaselineStatus::RESET:
        return "reset";
    case AdaptiveBaselineStatus::LEARNING:
        return "learning";
    case AdaptiveBaselineStatus::READY:
        return "ready";
    case AdaptiveBaselineStatus::OUTSIDE_ADAPTIVE_BAND:
        return "outside_adaptive_band";
    case AdaptiveBaselineStatus::HARD_LIMIT:
        return "hard_limit";
    case AdaptiveBaselineStatus::INVALID_SAMPLE:
        return "invalid_sample";
    }
    return "unknown";
}

size_t AdaptiveBaseline::persistentStateSize()
{
    return sizeof(AdaptiveBaselinePersistentState);
}
