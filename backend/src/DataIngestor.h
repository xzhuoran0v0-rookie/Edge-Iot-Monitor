#pragma once

#include <string>
#include <memory>
#include <vector>
#include "StorageEngine.h"

class DataFilter;
class AIQueryDispatcher;
class CloudSync;

/**
 * @brief 数据接入模块(HTTP 入口)
 *
 * 接受来自ESP32的数据
 *
 *   HTTP POST (/api/ingest)
 *        ↓
 *   JSON解析（parseJson）
 *        ↓
 *   数据校验（validate）
 *        ↓
 *   异常检测（DataFilter）
 *        ↓
 *   数据存储（StorageEngine）
 *        ↓
 *   AI分析（AIQueryDispatcher）
 *        ↓
 *   云同步（CloudSync）
 *
 */
class DataIngestor
{
public:
    /**
     * @brief 构造函数
     *
     * 所有核心模块通过引用传入，提高解耦性
     */
    DataIngestor(
        StorageEngine &storage,
        DataFilter &filter,
        AIQueryDispatcher &ai,
        CloudSync &cloud,
        double temp_min = -40.0, double temp_max = 85.0,
        double hum_min = 0.0, double hum_max = 100.0,
        std::vector<std::string> allowlist = {},
        std::string command_api_key = "");

    ~DataIngestor();

    /**
     * @brief 启动HTTP服务器(阻塞调用)
     *
     * @param port 监听端口，默认8080
     *
     * 该函数通常运行在主线程中，进入事件循环
     * 持续接受来自设备端的POST请求
     */
    bool start(const std::string &host = "0.0.0.0",
               int port = 8080,
               int max_connections = 32,
               int request_timeout_ms = 5000);

    /**
     * @brief 停止HTTP服务器
     *
     * graceful shutdown
     */
    void stop();

private:
    struct Impl;

    /**
     * @brief 处理一次数据上报请求
     *
     * @param raw_json 原始JSON字符串 (来自HTTP body)
     * @param respond_json 返回给客户端的JSON响应
     *
     * 流程：
     *  1.解析JSON
     *  2.校验数据合法性
     *  3.进行异常检测(IQR)
     *  4.写入数据库
     *  5.触发AI分析
     *  6.云同步(optional)
     *
     */
    void handleIngest(const std::string &raw_json,
                      std::string &respond_json,
                      int &status_code);

    void handleCreateCommand(const std::string &raw_json,
                             const std::string &api_key,
                             std::string &respond_json,
                             int &status_code);

    void handleNextCommand(const std::string &device_id,
                           std::string &respond_json,
                           int &status_code);

    void handleCommandAck(const std::string &raw_json,
                          std::string &respond_json,
                          int &status_code);

    /**
     * @brief 解析JSON数据
     *
     * @param raw_json 原始JSON数据
     * @param out_readings 输出：拆分后的多条读数（temperature/humidity）
     * @param error_msg 失败返回错误信息
     *
     * @return 是否解析成功
     */
    bool parseJson(const std::string &raw_json,
                   std::vector<SensorReading> &out_readings,
                   std::string &error_msg);

    /**
     * @brief 数据范围校验
     *
     * @param readings 输出：拆分后的多条读数
     * @param error_msg 失败返回错误信息
     *
     * @return 是否符合校验范围
     */
    bool validate(const std::vector<SensorReading> &readings,
                  std::string &error_msg);

    bool isDeviceAllowed(const std::string &device_id) const;
    bool isCommandApiAuthorized(const std::string &api_key) const;
    static bool isAllowedCommand(const std::string &command);

    //=========== 依赖模块 ==============//
    StorageEngine &storage_; ///< 数据库存储模块
    DataFilter &filter_;     ///< 异常检测模块(IQR算法)
    AIQueryDispatcher &ai_;  ///< 本地AI分析模块
    CloudSync &cloud_;       ///< 云端同步模块

    //=========== 校验范围（来自 AppConfig）============//
    double temp_min_, temp_max_;
    double hum_min_,  hum_max_;
    std::vector<std::string> allowlist_;
    std::string command_api_key_;

    std::unique_ptr<Impl> impl_;
};
