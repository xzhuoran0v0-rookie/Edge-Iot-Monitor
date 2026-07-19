#include "CloudSync.h"

#include <iostream>
#include <sstream>

// ─────────────────────────────────────────────
// 构造函数
// ─────────────────────────────────────────────
CloudSync::CloudSync(
    StorageEngine &storage,
    std::string endpoint,
    std::string project_id,
    std::string device_id,
    std::string credential,
    bool enabled)
    : storage_(storage)
    , endpoint_(std::move(endpoint))
    , project_id_(std::move(project_id))
    , device_id_(std::move(device_id))
    , credential_(std::move(credential))
    , enabled_(enabled)
{
}

// ─────────────────────────────────────────────
// 是否启用
// ─────────────────────────────────────────────
bool CloudSync::isEnabled() const
{
    return enabled_ && !endpoint_.empty() && !credential_.empty();
}

// ─────────────────────────────────────────────
// 数据入口（由 DataIngestor 调用）
// ─────────────────────────────────────────────
void CloudSync::onNewData(const std::vector<SensorReading> &readings)
{
    if (!isEnabled())
        return;

    pending_count_ += static_cast<int>(readings.size());

    // TODO: 等注册华为云后实现发送逻辑
    // 当前仅打印待发送条数，防止忘记对接
    static int log_count = 0;
    if (++log_count % 10 == 0)
    {
        std::cout << "[CloudSync] " << pending_count_
                  << " records pending (sendToCloud() not implemented yet)"
                  << std::endl;
    }
}

// ─────────────────────────────────────────────
// 健康检查（占位）
// ─────────────────────────────────────────────
bool CloudSync::healthCheck()
{
    if (!isEnabled())
        return false;

    // TODO: 向华为云 endpoint 发一次 GET /v5/iot/{project_id}/health
    std::cout << "[CloudSync] Health check skipped (not implemented)\n";
    return false;
}

// ─────────────────────────────────────────────
// 构建设备影子 JSON（占位）
// ─────────────────────────────────────────────
std::string CloudSync::buildShadowPayload(const SensorReading &reading)
{
    // TODO: 按华为云 IoTDA 设备影子格式序列化
    //
    // 预期格式（参考华为云文档）：
    // {
    //   "services": [{
    //     "service_id": "sensor",
    //     "properties": {
    //       "device_id":     "...",
    //       "sensor_type":   "...",
    //       "value":         25.1,
    //       "unit":          "C",
    //       "timestamp":     "2026-..."
    //     }
    //   }]
    // }
    //
    std::ostringstream ss;
    ss << "{\"services\":[{\"service_id\":\"sensor\",\"properties\":{"
       << "\"device_id\":\"" << reading.device_id << "\","
       << "\"sensor_type\":\"" << reading.sensor_type << "\","
       << "\"value\":" << reading.value << ","
       << "\"unit\":\"" << reading.unit << "\","
       << "\"timestamp\":\"" << reading.server_timestamp << "\""
       << "}}]}";
    return ss.str();
}

// ─────────────────────────────────────────────
// 发送到华为云（占位）
// ─────────────────────────────────────────────
bool CloudSync::sendToCloud(const std::string &payload)
{
    // TODO: 注册华为云后实现 HTTP POST
    //
    // 步骤：
    // 1. 按 HMAC-SHA256 对 payload 签名（使用 credential_）
    // 2. 构造 Authorization header
    // 3. POST https://{endpoint_}/v5/iot/{project_id_}/devices/{device_id_}/shadow
    // 4. 检查 HTTP 状态码
    //
    (void)payload;
    std::cout << "[CloudSync] sendToCloud() not implemented — "
              << "payload size=" << payload.size() << " bytes\n";
    return false;
}

// ─────────────────────────────────────────────
// 标记已同步（占位）
// ─────────────────────────────────────────────
void CloudSync::markSynced(const std::string &table_name, int last_id)
{
    // TODO: 写入 sync_status 表
    //
    // INSERT OR REPLACE INTO sync_status (table_name, last_synced_id)
    // VALUES (?, ?);
    //
    (void)table_name;
    (void)last_id;
}
