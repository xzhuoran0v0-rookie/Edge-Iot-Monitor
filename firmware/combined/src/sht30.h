#pragma once

#include <Arduino.h>
#include <Wire.h>

#define SHT30_SDA 17 //
#define SHT30_SCL 18 //
#define SHT30_ADDR 0x44

/**
 * @brief SHT30温湿度传感器驱动
 *
 * 功能：
 * - 初始化IIC
 * - 发送测试指令
 * - 读取并解析数据
 *
 */
class SHT30
{
public:
    /**
     * @brief 初始化
     * @return true=成功
     */
    static bool init();

    /**
     * @brief 读数温湿度
     * @param temp 输出温度（℃）
     * @param humi 输出湿度 (%RH)
     * @return true=读取成功
     */
    static bool read(float &temp, float &humi);

private:
    /**
     * @brief CRC8 校验
     * @param data 数据指针
     * @param len  数据长度
     * @return CRC8 zhi
     */
    static uint8_t crc8(const uint8_t *data, int len);
};