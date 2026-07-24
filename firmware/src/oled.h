#pragma once

#include <Arduino.h>
#include <Wire.h>

// OLED 使用第二条硬件 I2C 总线 (Wire1)，与 SHT30 (默认 Wire, GPIO17/18) 隔离
#define OLED_SDA 39   // Wire1 SDA
#define OLED_SCL 38   // Wire1 SCL
#define OLED_ADDR 0x3c

#define OLED_WIDTH 128
#define OLED_HEIGHT 64

/**
 * @brief SSD1315 OLED 显示驱动
 *
 * 功能：
 * - 初始化OLED
 * - 显示温湿度数据
 * - 显示AI分析结果
 *
 */
class OLED
{
public:
    /**
     * @brief 初始化OLED
     * @return ture=成功
     */
    static bool init();

    /**
     * @brief 清屏
     */
    static void clear();

    /**
     * @brief 显示传感器数据
     * @param temp 温度
     * @param humi 湿度
     *
     */
    static void showSensorData(float temp, float humi);

    /**
     * @brief 显示 AI 分析结果（截断显示）
     * @param result AI 输出文本
     */
    static void showAIResult(const String &result);

    /**
     * @brief 显示状态信息（WiFi、连接等）
     * @param msg 状态文本
     */
    static void showStatus(const String &msg);

private:
    static void sendCmd(uint8_t cmd);
    static void sendData(uint8_t *buf, size_t len);
    static void setCursor(uint8_t page, uint8_t col);

    // 字模数据（ASCII 5x7）
    static const uint8_t font5x7[][5];
    static void drawChar(char c);
    static void drawString(const String &s);

    static uint8_t cursor_page_;
    static uint8_t cursor_col_;
};