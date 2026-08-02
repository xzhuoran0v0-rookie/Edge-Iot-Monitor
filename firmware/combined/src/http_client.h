#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include "edge_reasoner.h"

/**
 * @brief HTTP POST 模块
 *
 * 职责：
 * - 构造 JSON payload
 * - POST 到后端 /api/ingest
 * - 返回响应状态
 */
class HttpClient
{
public:
    static void initActuators();

    static bool postSensorData(float temp, float humi,
                               const EdgeAssessment &assessment);

    static void pollAndApplyCommand();

    static bool backendReachable();
    static void resetBackoff();

private:
    static String buildJson(float temp, float humi,
                            const EdgeAssessment &assessment);
    static String commandBaseUrl();
    static void ackCommand(int commandId, const String &result);
    static bool applyCommand(const String &command, int durationMs);

    static uint8_t consecutiveFailures_;
    static unsigned long backoffUntilMs_;
};
