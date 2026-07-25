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

/**
 * @brief 一条 AI 叙述记录（analysis_log 的一行，不含 prompt）
 *
 * 状态页只需要模型说了什么和什么时候说的；prompt 体积大且对前端无用。
 */
struct AnalysisRecord
{
    std::string response;
    std::string timestamp;
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
     * @brief 每个 sensor_type 的最新一条读数
     *
     * 状态页要展示“现在是多少”，而不是最近 N 条里混在一起的多种量纲 ——
     * getRecentReadings(limit) 做不到这件事：温度更新频繁时会把湿度挤出窗口。
     *
     * @param device_id 设备ID
     * @return 每种 sensor_type 各一条，按 sensor_type 排序
     */
    std::vector<SensorReading> getLatestPerSensor(const std::string &device_id);

    /**
     * @brief 最近一次 AI 叙述
     *
     * @param device_id 设备ID
     * @param out 输出：命中时填充
     * @return 是否存在记录（该设备还没跑过叙述时返回 false）
     */
    bool getLatestAnalysis(const std::string &device_id, AnalysisRecord &out);

    /**
     * @brief 最近的异常事件
     *
     * @param device_id 设备ID
     * @param limit 返回数量上限
     * @return 按时间倒序；unit 字段为空（anomaly_events 不存单位）
     */
    std::vector<SensorReading> getRecentAnomalies(const std::string &device_id,
                                                  int limit);

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
