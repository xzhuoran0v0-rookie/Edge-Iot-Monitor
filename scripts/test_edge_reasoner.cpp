#include "../firmware/combined/src/edge_reasoner.h"

#include <cassert>
#include <cstring>
#include <iostream>

namespace
{
void addStable(EdgeReasoner &reasoner, float temperature, float humidity)
{
    for (unsigned long i = 0; i < 4; ++i)
        reasoner.add(temperature, humidity, i * 10000UL);
}

void expectState(const EdgeAssessment &assessment, const char *state)
{
    assert(std::strcmp(assessment.state, state) == 0);
    assert(assessment.confidence >= 0.0f);
    assert(assessment.confidence <= 1.0f);
}
}

int main()
{
    EdgeReasoner warmup;
    warmup.add(25.0f, 55.0f, 0);
    expectState(warmup.assess(), "WARMUP");

    EdgeReasoner normal;
    addStable(normal, 25.0f, 55.0f);
    expectState(normal.assess(), "NORMAL");

    EdgeReasoner humid;
    addStable(humid, 25.0f, 75.0f);
    expectState(humid.assess(), "HIGH_HUMIDITY");

    EdgeReasoner combinedRisk;
    addStable(combinedRisk, 31.0f, 75.0f);
    expectState(combinedRisk.assess(), "HEAT_HUMID_RISK");

    EdgeReasoner rising;
    rising.add(25.0f, 55.0f, 0);
    rising.add(25.4f, 55.0f, 10000);
    rising.add(25.8f, 55.0f, 20000);
    rising.add(26.2f, 55.0f, 30000);
    expectState(rising.assess(), "TEMP_RISING");

    EdgeReasoner unstable;
    unstable.add(25.0f, 55.0f, 0);
    unstable.add(25.1f, 55.2f, 10000);
    unstable.add(28.2f, 55.3f, 20000);
    unstable.add(28.1f, 55.4f, 30000);
    expectState(unstable.assess(), "UNSTABLE");

    std::cout << "edge reasoner tests passed\n";
    return 0;
}
