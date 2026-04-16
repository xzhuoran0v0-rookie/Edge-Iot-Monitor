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
    std::string timestamp; // insert itself
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
     * @brief 查询最近的N条数据
     * @param device_id 设备id
     * @param limit 返回数量
     * @return 数据列表(按时间排列)
     */
    std::vector<SensorReading> getRecentReadings(const std::string &device_id, int limit);

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
