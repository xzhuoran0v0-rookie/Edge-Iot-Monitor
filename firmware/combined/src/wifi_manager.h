#pragma once

#include<Arduino.h>
#include<WiFi.h>




/**
 * @brief WiFi 连接管理模块
 *
 * 职责：
 * - 连接 WiFi（阻塞直到成功或超时）
 * - 提供连接状态查询
 * - 断线自动重连
 */
class WiFiManager{
public:
    /**
     * @brief 初始化并连接 WiFi
     *
     * @param ssid     网络名称
     * @param password 网络密码
     * @param timeout_ms 连接超时（毫秒），默认 10s
     * @return true = 连接成功
     */
    static bool init(const char *ssid,
                    const char *password,
                uint32_t timeout_ms=10000);
    
    /**
     * @brief 检查当前是否已连接
     * @return true = 已连接
     */
    static bool isConnected();


     /**
     * @brief 断线重连（在 loop() 里定期调用）
     *
     * 行为：
     * - 已连接 → 直接返回
     * - 断线   → 尝试重连一次
     */
    static void reconnectIfNeeded();

    /**
     * @brief 获取本机 IP（用于串口调试）
     * @return IP 字符串，如 "192.168.1.105"
     */
    static String localIP();

private:
    static const char *ssid_;
    static const char *password_;
};