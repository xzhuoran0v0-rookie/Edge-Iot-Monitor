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

    /// 开机自检：短鸣一声。接线和触发电平不对，这里立刻听得出来。
    static void selfTestBuzzer();

    /// 蜂鸣器统一出口。电平由 config.h 的 BUZZER_ACTIVE_LEVEL 决定，
    /// ENABLE_BUZZER 为 0 时是空操作。
    static void setBuzzer(bool on);

    /// 本地阈值告警占用蜂鸣器期间，远程 buzzer 命令一律拒绝 ——
    /// 真实告警不该被一条网络命令按掉。
    static void setLocalAlarm(bool active);
    static bool localAlarm();

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

    static bool localAlarmActive_;
    static uint8_t consecutiveFailures_;
    static unsigned long backoffUntilMs_;
};
