#pragma once

#include <stddef.h>

/**
 * Compact, deterministic environmental assessment produced entirely on-device.
 *
 * The strings point to static program memory; callers may safely retain the
 * returned value until the next assessment. No heap allocation or network
 * connection is required.
 */
struct EdgeAssessment
{
    const char *state;
    const char *severity;
    float confidence;
    const char *reasonCode;
    const char *reason;
    const char *displayLabel;
};

class EdgeReasoner
{
public:
    static constexpr size_t WINDOW_SIZE = 12;

    void add(float temperature, float humidity, unsigned long capturedAtMs);
    EdgeAssessment assess() const;
    size_t sampleCount() const;

private:
    struct Sample
    {
        float temperature = 0.0f;
        float humidity = 0.0f;
        unsigned long capturedAtMs = 0;
    };

    const Sample &chronological(size_t index) const;
    static float clampConfidence(float value);

    Sample samples_[WINDOW_SIZE]{};
    size_t count_ = 0;
    size_t next_ = 0;
};
