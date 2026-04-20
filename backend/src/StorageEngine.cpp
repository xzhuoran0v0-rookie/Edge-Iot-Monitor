#include "StorageEngine.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>

/**
 * @brief 构造函数
 * 保存数据库路径
 */
StorageEngine::StorageEngine(const std::string &db_path)
    : db_path_(db_path) {}

/**
 * @brief 析构函数
 * 确保关闭时断开与数据库的连接
 */
StorageEngine::~StorageEngine()
{
    if (db_)
        sqlite3_close(db_);
}

/**
 * @brief 初始化数据库
 *
 * 步骤：
 * 1.打开SQLite数据库
 * 2.创建三张表
 *
 * @return 是否成功
 */
bool StorageEngine::init()
{
    // 初始化数据库文件(不存在将被创建)
    if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK)
    {
        std::cerr << "Can't open DataBase:" << sqlite3_errmsg(db_) << "\n";
        return false;
    }

    // Single source of truth: execute `sql/schema.sql` instead of duplicating schema here.
    // Support running from repo root ("sql/schema.sql") or from "backend/" ("../sql/schema.sql").
    const char *candidates[] = {"sql/schema.sql", "../sql/schema.sql"};
    std::string schemaPath;
    std::ifstream f;
    for (const char *p : candidates)
    {
        f.open(p, std::ios::in);
        if (f.is_open())
        {
            schemaPath = p;
            break;
        }
        f.clear();
    }

    if (!f.is_open())
    {
        std::cerr << "Can't open schema file (tried sql/schema.sql and ../sql/schema.sql)\n";
        return false;
    }

    std::ostringstream buf;
    buf << f.rdbuf();
    f.close();

    return execute(buf.str());
}

/**
 * @brief 插入一条传感器数据
 *
 * 直接拼接SQl字符串
 */
bool StorageEngine::insertReading(const SensorReading &r)
{
    // Use a prepared statement to avoid SQL injection and quoting issues.
    static constexpr const char *kSql =
        "INSERT INTO sensor_readings (device_id, sensor_type, value, unit, timestamp) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, r.device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, r.sensor_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, r.value);
    sqlite3_bind_text(stmt, 4, r.unit.c_str(), -1, SQLITE_TRANSIENT);
    const std::string &ts = !r.server_timestamp.empty() ? r.server_timestamp : r.timestamp;
    sqlite3_bind_text(stmt, 5, ts.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

/**
 * @brief 获取某个设备最近的 N 条数据
 *
 * @param device_id 设备ID
 * @param limit 返回条数
 *
 * @return SensorReading 列表
 *
 * 流程：
 * 1. prepare SQL（编译 SQL）
 * 2. step（逐行读取）
 * 3. column_xxx（取字段）
 * 4. finalize（释放资源）
 */

std::vector<SensorReading> StorageEngine::getRecentReadings(
    const std::string &device_id, int limit)
{
    std::vector<SensorReading> results;

    // Query with parameters to avoid SQL injection / quoting issues.
    static constexpr const char *kSql =
        "SELECT device_id, sensor_type, value, unit, timestamp "
        "FROM sensor_readings WHERE device_id = ? "
        "ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt = nullptr; // SQL 语句对象（prepared statement）

    // 编译 SQL
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    // 逐行读取结果
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        SensorReading r;

        // 从列中取数据（注意类型转换）
        r.device_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        r.sensor_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        r.value = sqlite3_column_double(stmt, 2);
        r.unit = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        r.timestamp = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        r.server_timestamp = r.timestamp;

        results.push_back(r);
    }

    // 释放 statement（非常重要）
    sqlite3_finalize(stmt);

    return results;
}

/**
 * @brief 执行通用 SQL（无返回结果）
 *
 * @param sql SQL 语句
 * @return 是否成功
 *
 * 内部使用 sqlite3_exec（适合 CREATE / INSERT 等简单语句）
 */
bool StorageEngine::execute(const std::string &sql)
{
    char *err = nullptr;

    // 执行 SQL
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
    {
        std::cerr << "SQL error: " << (err ? err : "(unknown)") << "\n";

        // SQLite 分配的错误信息需要手动释放
        if (err)
            sqlite3_free(err);

        return false;
    }

    return true;
}

static std::string nowIso8601Utc()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);

    std::tm utc_tm{};
#ifdef _WIN32
    gmtime_s(&utc_tm, &t);
#else
    gmtime_r(&t, &utc_tm);
#endif

    std::ostringstream ss;
    ss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

/**
 * @brief 写入 AI 分析结果
 *
 * @param device_id 设备ID
 * @param prompt    输入prompt
 * @param result    模型输出
 * @return 是否成功
 */
bool StorageEngine::insertAnalysisLog(const std::string &device_id,
                                      const std::string &prompt,
                                      const std::string &result)
{
    static constexpr const char *kSql =
        "INSERT INTO analysis_log (device_id, prompt, result, created_at) "
        "VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = nullptr;

    // 准备 SQL
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        std::cerr << "[Storage] prepare failed: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // 绑定参数
    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, result.c_str(), -1, SQLITE_TRANSIENT);

    // UTC 时间（ISO8601 + Z）
    const std::string now = nowIso8601Utc();
    sqlite3_bind_text(stmt, 4, now.c_str(), -1, SQLITE_TRANSIENT);

    // 执行
    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        std::cerr << "[Storage] insert failed: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}
