#pragma once

#include <string>
#include <vector>
#include "StorageEngine.h"

/**
 * @brief 云端数据同步模块
 *
 * 职责：
 * - 将本地 sensor_readings / anomaly_events / analysis_log 增量推送到华为云 IoTDA
 * - 发送失败时降级到本地重试队列
 * - 追踪各表的同步进度（last_synced_id）
 *
 * 当前状态：接口骨架，华为云 API 待注册后填充。
 */
class CloudSync
{
public:
    /**
     * @brief 构造函数
     * @param storage  数据存储接口（读未同步数据）
     * @param endpoint 华为云 IoTDA endpoint
     * @param project_id 华为云项目 ID
     * @param device_id  设备 ID（注册到 IoTDA 的设备标识）
     * @param credential 设备密钥（HMAC-SHA256 签名用，平台侧下发的 secret）
     * @param enabled    是否启用云同步
     */
    CloudSync(
        StorageEngine &storage,
        std::string endpoint,
        std::string project_id,
        std::string device_id,
        std::string credential,
        bool enabled = false);

    /**
     * @brief 将新入库的数据批量同步到云端
     *
     * 调用时机：
     * - 每次 DataIngestor::handleIngest 完成后调用
     * - 未来可由独立定时器线程驱动
     *
     * @param readings 本次写入的 SensorReading 列表
     */
    void onNewData(const std::vector<SensorReading> &readings);

    /**
     * @brief 检验云端连接是否正常
     * @return true 如果华为云 endpoint 可达且认证通过
     */
    bool healthCheck();

    /**
     * @brief 云端同步是否已启用
     * @return true 如果配置了有效的 endpoint 和 credential
     */
    bool isEnabled() const;

private:
    /**
     * @brief 将单条读数序列化为华为云设备影子 JSON
     *
     * @param reading 传感器读数
     * @return 华为云 IoTDA /v5/iot/{project_id}/devices/{device_id}/shadow 的请求体
     */
    std::string buildShadowPayload(const SensorReading &reading);

    /**
     * @brief 向华为云 IoTDA 发送 HTTP POST（占位）
     *
     * 实际发送：
     *   POST https://{endpoint}/v5/iot/{project_id}/devices/{device_id}/shadow
     *   Headers: Content-Type: application/json, Authorization: HMAC-SHA256...
     *
     * @param payload 请求体
     * @return true 发送成功
     */
    bool sendToCloud(const std::string &payload);

    /**
     * @brief 更新本地表中已同步的进度
     *
     * @param table_name 表名 ("sensor_readings" / "anomaly_events" / "analysis_log")
     * @param last_id    该表最后一条已同步记录的 id
     */
    void markSynced(const std::string &table_name, int last_id);

    // ---- 依赖 ----
    StorageEngine &storage_;

    // ---- 华为云配置 ----
    std::string endpoint_;
    std::string project_id_;
    std::string device_id_;
    std::string credential_;
    bool enabled_ = false;

    // ---- 内部计数器 ----
    int pending_count_ = 0;  ///< 自上次 sync 后累积的未发送条数
};
