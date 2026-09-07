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

// 感知周期现在是自适应的，所以 observe() 收的是时钟而不是"第几个样本"。
// 测试用一个固定步长的假时钟，一次调用前进一步，语义与原来的"一个样本"等价。
constexpr uint32_t kStepMs = 1000;
uint32_t testClockMs = 0;

AdaptiveBaselineResult obs(AdaptiveBaseline &baseline,
                           float temperatureC,
                           float humidityPct)
{
    testClockMs += kStepMs;
    return baseline.observe(temperatureC, humidityPct, testClockMs);
}

AdaptiveBaselineConfig testConfig()
{
    AdaptiveBaselineConfig config;
    // learnStableRoom() 走 8 步，其中第一步是种子、不计时，所以恰好累计 7 步。
    config.warmupMs = 7 * kStepMs;
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
            obs(baseline, temperature, humidity);
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
        obs(baseline, NAN, 50.0f);
    CHECK(invalid.status ==
          AdaptiveBaselineStatus::INVALID_SAMPLE);
    CHECK(baseline.learnedSampleCount() == 0);

    const AdaptiveBaselineResult hard =
        obs(baseline, 45.0f, 50.0f);
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
        obs(baseline, 40.0f, 55.0f);
    CHECK(outside.status ==
          AdaptiveBaselineStatus::OUTSIDE_ADAPTIVE_BAND);
    CHECK(outside.temperatureOutsideBand);
    CHECK(!outside.learned);
    CHECK(baseline.learnedSampleCount() == learnedBefore);

    const AdaptiveBaselineResult hardHumidity =
        obs(baseline, 25.0f, 95.0f);
    CHECK(hardHumidity.status ==
          AdaptiveBaselineStatus::HARD_LIMIT);
    CHECK(hardHumidity.humidityHardLimit);
    CHECK(baseline.learnedSampleCount() == learnedBefore);
}

void testBadFirstSampleCannotPoisonBaseline()
{
    AdaptiveBaseline baseline(testConfig());

    const AdaptiveBaselineResult badFirst =
        obs(baseline, 0.0f, 30.0f);
    CHECK(badFirst.validSample);
    CHECK(!badFirst.learned);
    CHECK(baseline.learnedSampleCount() == 0);

    const AdaptiveBaselineResult replacement =
        obs(baseline, 25.0f, 55.0f);
    CHECK(replacement.validSample);
    CHECK(!replacement.learned);
    CHECK(baseline.learnedSampleCount() == 0);

    const AdaptiveBaselineResult confirmed =
        obs(baseline, 25.1f, 55.2f);
    CHECK(confirmed.learned);
    CHECK(baseline.learnedSampleCount() == 2);

    for (int i = 0; i < 6; ++i)
        CHECK(obs(baseline, 25.0f, 55.0f).learned);

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

    CHECK(sizeof(state) == 40);
    CHECK(AdaptiveBaseline::persistentStateSize() == 40);

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
            obs(restored, 25.0f, 55.0f);
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
        CHECK(obs(constrained, 20.5f, 55.0f).learned == (i != 0));

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
        CHECK(obs(baseline, 25.0f, 55.0f).learned);
    CHECK(!baseline.persistenceDue(498));
    CHECK(baseline.persistenceDue(499));

    AdaptiveBaselinePersistentState state = baseline.exportState();
    AdaptiveBaselineConfig policyOnlyChange = testConfig();
    policyOnlyChange.persistEveryLearnedSamples = 100;
    policyOnlyChange.minPersistIntervalMs = 60000;
    AdaptiveBaseline changedPolicy(policyOnlyChange);
    CHECK(changedPolicy.restoreState(state, 0));
}

// A relocated device must not stay in BASELINE_SHIFT forever.
//
// Out-of-band samples are never learned, so before relearning existed the band
// could not move again once the environment changed: every sample was outside,
// nothing was learned, nothing was written back to NVS. A sustained departure
// now restarts learning, while a brief one does not.
void testSustainedDepartureRelearns()
{
    AdaptiveBaselineConfig config = testConfig();
    // 首个带外样本起算，所以第 5 个样本的时距是 4 步。
    config.relearnAfterOutsideMs = 4 * kStepMs;

    AdaptiveBaseline baseline(config);
    learnStableRoom(baseline);
    CHECK(baseline.ready());

    // A short excursion is an anomaly, not a move: it must not relearn, and
    // returning in-band must clear the count.
    for (int i = 0; i < 4; ++i)
        CHECK(obs(baseline, 31.0f, 55.0f).outsideAdaptiveBand);
    CHECK(baseline.ready());
    CHECK(obs(baseline, 24.8f, 54.6f).learned);

    // Sustained departure: the clock restarts, so it takes a further five
    // consecutive samples rather than one.
    for (int i = 0; i < 4; ++i)
        CHECK(obs(baseline, 31.0f, 55.0f).outsideAdaptiveBand);
    CHECK(baseline.ready());

    const AdaptiveBaselineResult relearn = obs(baseline, 31.0f, 55.0f);
    CHECK(!relearn.outsideAdaptiveBand);
    CHECK(!baseline.ready());
    CHECK(baseline.learnedSampleCount() == 0);
    CHECK(baseline.dirty());

    // It settles on the new environment.
    for (int i = 0; i < 10; ++i)
        obs(baseline, 31.0f + (i % 3) * 0.1f, 55.0f);
    CHECK(baseline.ready());
    const AdaptiveBaselineResult settled = baseline.snapshot();
    CHECK(settled.temperatureLowerC < 31.0f);
    CHECK(settled.temperatureUpperC > 31.0f);

    // Hard limits are untouched by relearning — the point of the whole design.
    CHECK(settled.temperatureUpperC < config.hardTempUpperC);
    CHECK(obs(baseline, 46.0f, 55.0f).hardLimitExceeded);

    // Relearning can be switched off entirely.
    AdaptiveBaselineConfig frozenConfig = testConfig();
    frozenConfig.relearnAfterOutsideMs = 0;
    AdaptiveBaseline frozen(frozenConfig);
    learnStableRoom(frozen);
    for (int i = 0; i < 50; ++i)
        CHECK(obs(frozen, 31.0f, 55.0f).outsideAdaptiveBand);
    CHECK(frozen.ready());
}

// 感知周期是自适应的，所以"预热四分钟"和"持续偏离一小时"必须按时间成立，不能
// 按样本数——否则环境一安静、采样一变慢，这两个判据的含义就跟着漂了。这正是
// 把计数换成时钟的全部理由，所以它需要一条自己的测试。
void testWarmupIsMeasuredInTimeNotSamples()
{
    const AdaptiveBaselineConfig config = testConfig(); // warmupMs = 7 步

    // 慢采样：每次跨 5 步。三次调用（首次只对表、不计时）即越过预热门槛。
    AdaptiveBaseline slow(config);
    uint32_t slowClock = 0;
    for (int i = 0; i < 3; ++i)
    {
        slowClock += 5 * kStepMs;
        slow.observe(24.8f + (i % 2) * 0.1f, 54.5f, slowClock);
    }
    CHECK(slow.ready());
    // 而且是靠时间到的，不是靠样本数——按旧口径这点样本远远不够。
    CHECK(slow.learnedSampleCount() < 7);

    // 同样三次调用，快采样下时间不够，就不该就绪。
    AdaptiveBaseline fast(config);
    uint32_t fastClock = 0;
    for (int i = 0; i < 3; ++i)
    {
        fastClock += kStepMs;
        fast.observe(24.8f + (i % 2) * 0.1f, 54.5f, fastClock);
    }
    CHECK(!fast.ready());

    // 反过来也要挡住：调用方停摆十分钟，不能靠一个样本把预热跳完。
    AdaptiveBaselineConfig firmwareLike = testConfig();
    firmwareLike.warmupMs = 4UL * 60UL * 1000UL; // 与固件一致的 4 min
    AdaptiveBaseline stalled(firmwareLike);
    stalled.observe(24.8f, 54.5f, 0);
    stalled.observe(24.9f, 54.6f, 10UL * 60UL * 1000UL);
    CHECK(!stalled.ready());
}
}

int main()
{
    testLearningAndBounds();
    testBadFirstSampleCannotPoisonBaseline();
    testPersistenceRoundTripAndThrottle();
    testResetPersistence();
    testNarrowConfigurationAndMillisWrap();
    testSustainedDepartureRelearns();
    testWarmupIsMeasuredInTimeNotSamples();

    if (failures != 0)
    {
        std::cerr << failures << " adaptive baseline test(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "adaptive baseline host tests passed\n";
    return EXIT_SUCCESS;
}
