#pragma once

#include <Arduino.h>

struct IotdaBuzzerCommand
{
    bool valid = false;
    bool turnOn = false;
    int durationMs = 0;
    String pattern = "none";
};

class IotdaClient
{
public:
    static void logPlannedTopics();
    static String propertyReportTopic();
    static String commandSubscribeTopic();
    static String commandResponseTopic(const String &requestId);
    static String buildPropertyReport(float temp, float humi);
    static IotdaBuzzerCommand parseBuzzerCommand(const String &payload);
    static String buildCommandResponse(bool ok, const String &result);
};
