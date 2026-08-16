#pragma once
#include <string>
#include <vector>
#include "sqlite3.h"

/**
 * @brief 单条传感器数据（从 ESP32 或模拟器上传）
 *
 * 这一层是“数据模型”（Data Model），对应数据库中的一行记录
 */
struct SensorReading
{
    std::string device_id;
    std::string sensor_type;
    double value;
    std::string unit;
    // Backward-compatible field used by some utilities (e.g. ingest_stdin.cpp).
    // Prefer server_timestamp (UTC, ISO8601 with 'Z') for storage/query/prompt.
    std::string timestamp;

    // UTC server timestamp (ISO8601 + 'Z'), preferred for storage/query/prompt.
    std::string server_timestamp;

    // Optional device-provided timestamp (format depends on device).
    std::string device_timestamp;
};

struct DeviceCommand
{
    int id = 0;
    std::string device_id;
    std::string command;
    int duration_ms = 0;
    std::string status;
};

/**
 * @brief 设备端 EdgeReasoner 的评估结果（随读数一起上报）
 *
 * 这是设备自己得出的结论，不是服务端算的。服务端只负责落库和转发，
 * 不重新推理，也不修改判定 —— 否则网页显示的就不是芯片真正的判断了。
 */
struct EdgeAssessment
{
    std::string device_id;
    std::string state;
    std::string severity;
    double confidence = 0.0;
    std::string reason_code;
    std::string reason;
    std::string timestamp; ///< 该状态首次出现的时间（UTC ISO8601 + 'Z'）
};

/**
 * @brief 存储引擎（负责和 SQLite 数据库交互）
 *
 * 职责：
 * 1. 打开 / 初始化数据库
 * 2. 建表（init）
 * 3. 插入数据（insertReading）
 * 4. 查询数据（getRecentReadings）
 *
 */
class StorageEngine
{
public:
    /**
     * @brief 构造函数
     * @param db_path SQLite 数据库文件路径
     */
    explicit StorageEngine(const std::string &db_path);

    /**
     * @brief 析构函数
     */
    ~StorageEngine();

    /**
     * @brief 初始化数据库（建表）
     * @return 是否成功
     *
     */
    bool init();

    /**
     * @brief 插入一条传感器数据
     * @param reading 一条完整的传感器数据
     * @return 是否成功
     */
    bool insertReading(const SensorReading &reading);

    /**
     * @brief 持久化一条 IQR 异常检测事件
     * @param reading 被 DataFilter 判定为异常的 SensorReading
     * @return 是否插入成功
     */
    bool insertAnomaly(const SensorReading &reading);

    /**
     * @brief 查询最近的N条数据
     * @param device_id 设备id
     * @param limit 返回数量
     * @return 数据列表(按时间排列)
     */
    std::vector<SensorReading> getRecentReadings(const std::string &device_id, int limit);

    /**
     * @brief 写入 AI 分析结果
     *
     * @param device_id 设备ID
     * @param prompt    输入prompt
     * @param result    模型输出
     * @return 是否成功
     */
    bool insertAnalysisLog(const std::string &device_id,
                           const std::string &prompt,
                           const std::string &result);

    /**
     * @brief 记录设备端推理结果（仅在状态变化时落库）
     *
     * 设备每个上报周期都会带上评估结果，稳态下连续几百条完全相同。
     * 全存下来除了撑大表没有信息量，所以只在 state/severity/reason_code
     * 相对上一条发生变化时插入 —— 表里因此是一条状态变迁时间线，
     * 每行的 timestamp 就是该状态的起始时刻。
     *
     * @return 是否成功（未变化时不插入，同样返回 true）
     */
    bool insertEdgeAssessment(const EdgeAssessment &assessment);

    /**
     * @brief 读取某设备最新的推理结果
     * @return 是否查到（设备从未上报过 edge 字段时返回 false）
     */
    bool getLatestEdgeAssessment(const std::string &device_id,
                                 EdgeAssessment &out);

    bool enqueueDeviceCommand(const std::string &device_id,
                              const std::string &command,
                              int duration_ms,
                              int *command_id);

    bool getPendingDeviceCommand(const std::string &device_id,
                                 DeviceCommand &command);

    bool ackDeviceCommand(const std::string &device_id,
                          int command_id,
                          const std::string &result);

private:
    std::string db_path_;   // 数据库文件路径
    sqlite3 *db_ = nullptr; // SQLite 数据库连接句柄

    /**
     * @brief 执行通用SQL
     * @param sql SQL 语句(CREATE/INSERT/DELETE,etc.)
     * @return 是否成功
     *
     * 用途：
     * -init()建表
     * -简单SQl执行
     */

    bool execute(const std::string &sql);

};
