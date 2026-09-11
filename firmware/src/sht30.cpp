#include "sht30.h"

namespace
{
uint8_t sensor_address = SHT30_ADDR_PRIMARY;

bool respondsAt(uint8_t address)
{
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}
} // namespace

bool SHT30::init()
{
    Wire.begin(SHT30_SDA, SHT30_SCL);
    Wire.setClock(100000);
    delay(20);

    if (respondsAt(SHT30_ADDR_PRIMARY))
    {
        sensor_address = SHT30_ADDR_PRIMARY;
    }
    else if (respondsAt(SHT30_ADDR_SECONDARY))
    {
        sensor_address = SHT30_ADDR_SECONDARY;
    }
    else
    {
        Serial.print("[SHT30] No ACK at 0x44 or 0x45 on SDA=");
        Serial.print(SHT30_SDA);
        Serial.print(" SCL=");
        Serial.println(SHT30_SCL);
        return false;
    }

    // 发送复位指令
    Wire.beginTransmission(sensor_address);
    Wire.write(0x30);
    Wire.write(0xA2);
    int err = Wire.endTransmission();

    if (err != 0)
    {
        Serial.print("[SHT30] Reset failed, I2C error: ");
        Serial.println(err);
        return false;
    }

    delay(100);
    Serial.print("[SHT30] Init OK at address 0x");
    Serial.println(sensor_address, HEX);
    return true;
}

bool SHT30::read(float &temp, float &humi)
{
    // 发送单次测试指令
    Wire.beginTransmission(sensor_address);
    Wire.write(0x2c);
    Wire.write(0x06);
    if (Wire.endTransmission() != 0)
    {
        Serial.println("[SHT30] Measurement command failed");
        return false;
    }
    delay(100);

    // 读取6字节：温度(2) + CRC(1) + 湿度(2) + CRC(1)
    Wire.requestFrom(sensor_address, static_cast<uint8_t>(6));
    if (Wire.available() != 6)
    {
        Serial.println("[SHT30] read failed");
        return false;
    }

    uint8_t buf[6];
    for (int i = 0; i < 6; i++)
    {
        buf[i] = Wire.read();
    }

    // CRC校验
    if (SHT30::crc8(buf, 2) != buf[2] || SHT30::crc8(buf + 3, 2) != buf[5])
    {
        Serial.println("[SHT30] CRC error");
        return false;
    }

    // 转换公式
    uint16_t raw_temp = (buf[0] << 8) | buf[1];
    uint16_t raw_humi = (buf[3] << 8) | buf[4];

    temp = -45.0f + 175.0f * raw_temp / 65535.0f;
    humi = 100.0f * raw_humi / 65535.0f;

    return true;
}

uint8_t SHT30::crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc <<= 1;
        }
    }
    return crc;
}
