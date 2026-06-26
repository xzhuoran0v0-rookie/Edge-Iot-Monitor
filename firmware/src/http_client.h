#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

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

    /**
     * @brief 发送传感器数据
     *
     * @param temp 温度
     * @param humi 湿度
     * @return true = 成功（HTTP 200）
     */
    static bool postSensorData(float temp, float humi);

    static void pollAndApplyCommand();

private:
    /**
     * @brief 构造 JSON 字符串
     *
     * @param temp 温度
     * @param humi 湿度
     * @return JSON 字符串
     */
    static String buildJson(float temp, float humi);
    static String commandBaseUrl();
    static void ackCommand(int commandId, const String &result);
    static bool applyCommand(const String &command, int durationMs);
};
