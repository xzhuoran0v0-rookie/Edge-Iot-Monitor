#include "adaptive_baseline.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
int failures = 0;

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n";  \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

AdaptiveBaselineConfig testConfig()
{
    AdaptiveBaselineConfig config;
    config.warmupSamples = 8;
    config.persistEveryLearnedSamples = 4;
    config.minPersistIntervalMs = 1000;
    return config;
}

void learnStableRoom(AdaptiveBaseline &baseline)
{
    for (int i = 0; i < 8; ++i)
    {
        const float temperature = 24.8f + (i % 3) * 0.1f;
        const float humidity = 54.5f + (i % 2) * 0.4f;
        const AdaptiveBaselineResult result =
            baseline.observe(temperature, humidity);
        CHECK(result.validSample);
        CHECK(result.learned == (i != 0));
    }
}

void testLearningAndBounds()
{
    AdaptiveBaseline baseline(testConfig());
    CHECK(!baseline.ready());
    CHECK(baseline.progressPercent() == 0);
    CHECK(baseline.snapshot().status ==
          AdaptiveBaselineStatus::RESET);

    const AdaptiveBaselineResult invalid =
        baseline.observe(NAN, 50.0f);
    CHECK(invalid.status ==
          AdaptiveBaselineStatus::INVALID_SAMPLE);
    CHECK(baseline.learnedSampleCount() == 0);

    const AdaptiveBaselineResult hard =
        baseline.observe(45.0f, 50.0f);
    CHECK(hard.status == AdaptiveBaselineStatus::HARD_LIMIT);
    CHECK(hard.temperatureHardLimit);
    CHECK(!hard.learned);
    CHECK(baseline.learnedSampleCount() == 0);

    learnStableRoom(baseline);
    const AdaptiveBaselineResult ready = baseline.snapshot();
    CHECK(ready.ready);
    CHECK(ready.progressPct == 100);
    CHECK(ready.temperatureLowerC >
          baseline.config().hardTempLowerC);
    CHECK(ready.temperatureUpperC <
          baseline.config().hardTempUpperC);
    CHECK(ready.humidityLowerPct >
          baseline.config().hardHumidityLowerPct);
    CHECK(ready.humidityUpperPct <
          baseline.config().hardHumidityUpperPct);
    CHECK(baseline.persistenceDue(0));

    const uint32_t learnedBefore = baseline.learnedSampleCount();
    const AdaptiveBaselineResult outside =
        baseline.observe(40.0f, 55.0f);
    CHECK(outside.status ==
          AdaptiveBaselineStatus::OUTSIDE_ADAPTIVE_BAND);
    CHECK(outside.temperatureOutsideBand);
    CHECK(!outside.learned);
    CHECK(baseline.learnedSampleCount() == learnedBefore);

    const AdaptiveBaselineResult hardHumidity =
        baseline.observe(25.0f, 95.0f);
    CHECK(hardHumidity.status ==
          AdaptiveBaselineStatus::HARD_LIMIT);
    CHECK(hardHumidity.humidityHardLimit);
    CHECK(baseline.learnedSampleCount() == learnedBefore);
}

void testBadFirstSampleCannotPoisonBaseline()
{
    AdaptiveBaseline baseline(testConfig());

    const AdaptiveBaselineResult badFirst =
        baseline.observe(0.0f, 30.0f);
    CHECK(badFirst.validSample);
    CHECK(!badFirst.learned);
    CHECK(baseline.learnedSampleCount() == 0);

    const AdaptiveBaselineResult replacement =
        baseline.observe(25.0f, 55.0f);
    CHECK(replacement.validSample);
    CHECK(!replacement.learned);
    CHECK(baseline.learnedSampleCount() == 0);

    const AdaptiveBaselineResult confirmed =
        baseline.observe(25.1f, 55.2f);
    CHECK(confirmed.learned);
    CHECK(baseline.learnedSampleCount() == 2);

    for (int i = 0; i < 6; ++i)
        CHECK(baseline.observe(25.0f, 55.0f).learned);

    const AdaptiveBaselineResult ready = baseline.snapshot();
    CHECK(ready.ready);
    CHECK(std::fabs(ready.temperatureCenterC - 25.0f) < 0.2f);
    CHECK(std::fabs(ready.humidityCenterPct - 55.0f) < 0.3f);
}

void testPersistenceRoundTripAndThrottle()
{
    AdaptiveBaseline source(testConfig());
    learnStableRoom(source);
    const AdaptiveBaselinePersistentState state =
        source.exportState();

    CHECK(sizeof(state) == 36);
    CHECK(AdaptiveBaseline::persistentStateSize() == 36);

    AdaptiveBaseline restored(testConfig());
    CHECK(restored.restoreState(state, 200));
    CHECK(restored.ready());
    CHECK(!restored.dirty());
    CHECK(!restored.persistenceDue(1200));

    const AdaptiveBaselineResult sourceSnapshot = source.snapshot();
    const AdaptiveBaselineResult restoredSnapshot =
        restored.snapshot();
    CHECK(std::fabs(sourceSnapshot.temperatureCenterC -
                    restoredSnapshot.temperatureCenterC) < 0.0001f);
    CHECK(std::fabs(sourceSnapshot.humidityCenterPct -
                    restoredSnapshot.humidityCenterPct) < 0.0001f);

    for (int i = 0; i < 4; ++i)
    {
        const AdaptiveBaselineResult result =
            restored.observe(25.0f, 55.0f);
        CHECK(result.learned);
    }
    CHECK(!restored.persistenceDue(1199));
    CHECK(restored.persistenceDue(1200));
    restored.markPersisted(1200);
    CHECK(!restored.dirty());
    CHECK(!restored.persistenceDue(5000));

    AdaptiveBaselinePersistentState corrupt = state;
    corrupt.temperatureCenterC += 1.0f;
    AdaptiveBaseline unchanged(testConfig());
    CHECK(!unchanged.restoreState(corrupt, 0));
    CHECK(unchanged.learnedSampleCount() == 0);

    AdaptiveBaselineConfig changedConfig = testConfig();
    changedConfig.hardTempUpperC = 42.0f;
    AdaptiveBaseline wrongConfig(changedConfig);
    CHECK(!wrongConfig.restoreState(state, 0));
}

void testResetPersistence()
{
    AdaptiveBaseline baseline(testConfig());
    learnStableRoom(baseline);
    baseline.markPersisted(100);

    baseline.reset();
    CHECK(!baseline.ready());
    CHECK(baseline.learnedSampleCount() == 0);
    CHECK(baseline.snapshot().status ==
          AdaptiveBaselineStatus::RESET);
    CHECK(baseline.persistenceDue(101));

    const AdaptiveBaselinePersistentState resetState =
        baseline.exportState();
    AdaptiveBaseline restored(testConfig());
    CHECK(restored.restoreState(resetState, 101));
    CHECK(!restored.ready());
    CHECK(restored.learnedSampleCount() == 0);
    CHECK(restored.snapshot().status ==
          AdaptiveBaselineStatus::RESET);
}

void testNarrowConfigurationAndMillisWrap()
{
    AdaptiveBaselineConfig narrow = testConfig();
    narrow.physicalTempLowerC = 20.0f;
    narrow.physicalTempUpperC = 21.0f;
    narrow.hardTempLowerC = -100.0f; // Forces safe sanitization.
    narrow.hardTempUpperC = 200.0f;
    narrow.tempHardGuardC = 10.0f;
    narrow.minTempHalfBandC = 5.0f;
    narrow.maxTempHalfBandC = 10.0f;

    AdaptiveBaseline constrained(narrow);
    for (int i = 0; i < 8; ++i)
        CHECK(constrained.observe(20.5f, 55.0f).learned == (i != 0));

    const AdaptiveBaselineResult constrainedResult =
        constrained.snapshot();
    CHECK(constrainedResult.temperatureLowerC <
          constrainedResult.temperatureUpperC);
    CHECK(constrainedResult.temperatureLowerC >
          constrained.config().hardTempLowerC);
    CHECK(constrainedResult.temperatureUpperC <
          constrained.config().hardTempUpperC);

    AdaptiveBaseline baseline(testConfig());
    learnStableRoom(baseline);
    baseline.markPersisted(UINT32_MAX - 500U);
    for (int i = 0; i < 4; ++i)
        CHECK(baseline.observe(25.0f, 55.0f).learned);
    CHECK(!baseline.persistenceDue(498));
    CHECK(baseline.persistenceDue(499));

    AdaptiveBaselinePersistentState state = baseline.exportState();
    AdaptiveBaselineConfig policyOnlyChange = testConfig();
    policyOnlyChange.persistEveryLearnedSamples = 100;
    policyOnlyChange.minPersistIntervalMs = 60000;
    AdaptiveBaseline changedPolicy(policyOnlyChange);
    CHECK(changedPolicy.restoreState(state, 0));
}
}

int main()
{
    testLearningAndBounds();
    testBadFirstSampleCannotPoisonBaseline();
    testPersistenceRoundTripAndThrottle();
    testResetPersistence();
    testNarrowConfigurationAndMillisWrap();

    if (failures != 0)
    {
        std::cerr << failures << " adaptive baseline test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "adaptive baseline host tests passed\n";
    return EXIT_SUCCESS;
}
