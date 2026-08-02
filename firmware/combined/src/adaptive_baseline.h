#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Configuration for AdaptiveBaseline.
 *
 * The hard limits are immutable for the lifetime of a baseline instance and
 * are checked before any learning. Adaptive bands are always clamped inside
 * those limits, so a long-running abnormal condition cannot train the hard
 * safety boundary away.
 */
struct AdaptiveBaselineConfig
{
    // Number of accepted samples required before adaptive-band decisions start.
    uint16_t warmupSamples = 24;

    // Persistence throttle. The module never writes flash itself.
    uint16_t persistEveryLearnedSamples = 60;
    uint32_t minPersistIntervalMs = 15UL * 60UL * 1000UL;

    // Sensor values outside these physical ranges are invalid and never learned.
    float physicalTempLowerC = -40.0f;
    float physicalTempUpperC = 125.0f;
    float physicalHumidityLowerPct = 0.0f;
    float physicalHumidityUpperPct = 100.0f;

    // Fixed safety boundaries. Crossing either boundary is always reported.
    float hardTempLowerC = -10.0f;
    float hardTempUpperC = 45.0f;
    float hardHumidityLowerPct = 5.0f;
    float hardHumidityUpperPct = 95.0f;

    // Adaptive bands stay this far inside the corresponding hard boundary.
    float tempHardGuardC = 0.5f;
    float humidityHardGuardPct = 2.0f;

    // Bounded adaptive half-widths around the learned center.
    float minTempHalfBandC = 1.5f;
    float maxTempHalfBandC = 5.0f;
    float minHumidityHalfBandPct = 5.0f;
    float maxHumidityHalfBandPct = 15.0f;
    float deviationMultiplier = 3.0f;

    // Slow post-warmup learning prevents ordinary drift from moving the band
    // rapidly. Samples outside the adaptive band are not learned at all.
    float centerLearningRate = 0.02f;
    float deviationLearningRate = 0.05f;

    // Winsorization used only while collecting the initial baseline. It limits
    // how much one otherwise hard-safe sample can move the initial center.
    float warmupTempDeltaClampC = 3.0f;
    float warmupHumidityDeltaClampPct = 10.0f;
};

enum class AdaptiveBaselineStatus : uint8_t
{
    RESET = 0,
    LEARNING,
    READY,
    OUTSIDE_ADAPTIVE_BAND,
    HARD_LIMIT,
    INVALID_SAMPLE
};

/**
 * Result returned after each observation, or by snapshot().
 *
 * `validSample` means the values are finite and inside the sensor's physical
 * range. A hard-limit sample can therefore be valid but is never learned.
 */
struct AdaptiveBaselineResult
{
    AdaptiveBaselineStatus status = AdaptiveBaselineStatus::RESET;
    bool validSample = false;
    bool learned = false;
    bool ready = false;
    bool hardLimitExceeded = false;
    bool outsideAdaptiveBand = false;
    bool temperatureHardLimit = false;
    bool humidityHardLimit = false;
    bool temperatureOutsideBand = false;
    bool humidityOutsideBand = false;
    uint8_t progressPct = 0;
    uint32_t learnedSamples = 0;

    float temperatureCenterC = 0.0f;
    float humidityCenterPct = 0.0f;
    float temperatureLowerC = 0.0f;
    float temperatureUpperC = 0.0f;
    float humidityLowerPct = 0.0f;
    float humidityUpperPct = 0.0f;
};

/**
 * Stable, compact blob for Preferences::putBytes()/getBytes().
 *
 * The blob contains a format version, a configuration fingerprint, and a
 * checksum. Do not edit fields before restoreState(); a changed hard-limit or
 * learning configuration intentionally invalidates an older blob.
 */
struct AdaptiveBaselinePersistentState
{
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t size = 0;
    uint32_t configSignature = 0;
    uint32_t learnedSamples = 0;
    float temperatureCenterC = 0.0f;
    float humidityCenterPct = 0.0f;
    float temperatureDeviationC = 0.0f;
    float humidityDeviationPct = 0.0f;
    uint32_t checksum = 0;
};

/**
 * Incremental, bounded environmental baseline for temperature and humidity.
 *
 * Characteristics:
 * - O(1) CPU and fixed RAM per sample; no sample history is retained.
 * - No heap allocation and no dependency on Arduino APIs.
 * - Invalid, hard-limit, and post-warmup out-of-band samples are not learned.
 * - Hard boundaries remain independent of the learned center and deviation.
 * - Persistence is caller-controlled and throttled; this class never writes
 *   NVS or flash.
 *
 * Typical NVS flow:
 *
 *   AdaptiveBaseline baseline;
 *   AdaptiveBaselinePersistentState saved;
 *   if (prefs.getBytes("envbase", &saved, sizeof(saved)) == sizeof(saved))
 *       baseline.restoreState(saved, millis());
 *
 *   AdaptiveBaselineResult result = baseline.observe(temp, humidity);
 *   if (baseline.persistenceDue(millis())) {
 *       const auto state = baseline.exportState();
 *       if (prefs.putBytes("envbase", &state, sizeof(state)) == sizeof(state))
 *           baseline.markPersisted(millis());
 *   }
 *
 * Call markPersisted() only after the NVS operation succeeds.
 */
class AdaptiveBaseline
{
public:
    AdaptiveBaseline();
    explicit AdaptiveBaseline(const AdaptiveBaselineConfig &config);

    /**
     * Process one sensor sample and return the updated baseline assessment.
     */
    AdaptiveBaselineResult observe(float temperatureC, float humidityPct);

    /**
     * Return current progress, bands, and status without changing learning.
     */
    AdaptiveBaselineResult snapshot() const;

    /**
     * Clear all learned values. The reset is marked persistence-due so the
     * caller can overwrite/remove an older NVS blob exactly once.
     */
    void reset();

    bool ready() const;
    uint8_t progressPercent() const;
    uint32_t learnedSampleCount() const;
    const AdaptiveBaselineConfig &config() const;

    /**
     * Export a checksummed fixed-size NVS blob. This does not mark it saved.
     */
    AdaptiveBaselinePersistentState exportState() const;

    /**
     * Restore a blob if format, checksum, config, and numeric bounds are valid.
     * Invalid input leaves the current in-memory baseline unchanged.
     */
    bool restoreState(const AdaptiveBaselinePersistentState &state,
                      uint32_t nowMs);

    /**
     * True only when state is dirty and the persistence throttle permits a
     * write. First transition to READY and reset are each eligible once.
     */
    bool persistenceDue(uint32_t nowMs) const;

    /**
     * Notify the module that exportState() was successfully stored.
     */
    void markPersisted(uint32_t nowMs);

    bool dirty() const;

    static const char *statusName(AdaptiveBaselineStatus status);
    static size_t persistentStateSize();

private:
    static AdaptiveBaselineConfig sanitizeConfig(
        const AdaptiveBaselineConfig &config);
    static uint32_t calculateConfigSignature(
        const AdaptiveBaselineConfig &config);
    static uint32_t calculateStateChecksum(
        const AdaptiveBaselinePersistentState &state);

    void clearRuntimeState(bool persistenceRequired);
    AdaptiveBaselineResult makeResult(
        AdaptiveBaselineStatus status) const;
    void calculateBands(float &temperatureLowerC,
                        float &temperatureUpperC,
                        float &humidityLowerPct,
                        float &humidityUpperPct) const;
    void learnWarmup(float temperatureC, float humidityPct);
    void learnReady(float temperatureC, float humidityPct);

    AdaptiveBaselineConfig config_;
    uint32_t configSignature_ = 0;
    uint32_t learnedSamples_ = 0;
    float temperatureCenterC_ = 0.0f;
    float humidityCenterPct_ = 0.0f;
    float temperatureDeviationC_ = 0.0f;
    float humidityDeviationPct_ = 0.0f;
    AdaptiveBaselineStatus lastStatus_ = AdaptiveBaselineStatus::RESET;
    bool seedPending_ = false;
    float seedTemperatureC_ = 0.0f;
    float seedHumidityPct_ = 0.0f;

    uint32_t learnedSincePersist_ = 0;
    uint32_t lastPersistMs_ = 0;
    bool persistedOnce_ = false;
    bool dirty_ = false;
    bool forcePersistence_ = false;
};
