#pragma once

#include <string>
#include <vector>
#include <unordered_map>

#include "StorageEngine.h"

/**
 * @brief AI分析调度模块（基于Ollama本地大模型）
 *
 * 功能：
 * - 接收来自 DataIngestor 的新数据（统一为 vector<SensorReading>）
 * - 按 device_id 统计数据条数
 * - 达到 trigger_count 后触发一次 AI 分析
 * - 从 StorageEngine 查询历史数据
 * - 构建 prompt → 调用 Ollama → 存入 analysis_log
 *
 * 设计原则：
 * - 与 StorageEngine 解耦（仅通过接口访问数据）
 * - 支持多传感器类型（temperature / humidity / future扩展）
 * - 不依赖具体数据库实现（SQLite / MySQL 均可）
 */
class AIQueryDispatcher
{
public:
    /**
     * @brief 构造函数
     *
     * @param storage 数据存储接口(依赖注入)
     * @param ollama_url ollama服务器地址
     * @param model 使用模型名称
     * @param trigger_conut 每累计多少条数据触发一次分析
     * @param window_size 每次分析使用的历史数据条数
     */
    explicit AIQueryDispatcher(
        StorageEngine &storage,
        std::string ollama_url = "http://localhost.11434",
        std::string model = "qwen2.5:3b-instruct-q4_K_M",
        int trigger_count = 20,
        int window_size = 10);

    /**
     * @brief 接收新数据（由 DataIngestor 调用）
     *
     * 行为：
     * - 按 device_id 累加数据条数
     * - 当累计达到 trigger_count 时触发 AI 分析
     *
     * @param readings 新入库的数据（可能包含多个传感器）
     *
     * 设计说明：
     * - 使用 vector 统一数据模型（避免 temp/humi 分裂）
     * - 支持未来增加更多传感器类型
     */
    void onNewData(const std::vector<SensorReading> &readings);

private:
    /**
     * @brief 构建发送给 LLM 的 prompt
     *
     * @param device_id 设备ID
     * @param history   历史数据（最近 window_size 条）
     * @return prompt字符串
     *
     * 设计说明：
     * - prompt包含多传感器数据
     * - 由模型自行理解数据模式
     */
    std::string buildPrompt(const std::string &device_id,
                            const std::vector<SensorReading> &history);

    /**
     * @brief 调用 Ollama 推理接口
     *
     * @param prompt 输入提示词
     * @return 模型输出文本
     *
     * 调用接口：
     *   POST /api/generate
     */
    std::string callOllama(const std::string &prompt);

    /**
     * @brief 将AI分析结果写入数据库
     *
     * @param device_id 设备ID
     * @param prompt    输入prompt
     * @param result    模型输出
     */
    void saveResult(const std::string &device_id,
                    const std::string &prompt,
                    const std::string &result);

    /**
     * @brief 执行一次完整分析流程
     *
     * 流程：
     *   1. 查询历史数据
     *   2. 构建 prompt
     *   3. 调用 Ollama
     *   4. 写入 analysis_log
     */
    void runAnalysis(const std::string &device_id);

private:
    StorageEngine &storage_; ///< 数据存储接口（不使用 friend）

    std::string ollama_url_; ///< Ollama服务地址
    std::string model_;      ///< 模型名称

    int trigger_count_; ///< 触发分析阈值（数据条数）
    int window_size_;   ///< 分析窗口大小

    /**
     * @brief 每个设备的计数器
     *
     * key   = device_id
     * value = 自上次分析后累计的数据条数
     */
    std::unordered_map<std::string, int> counters_;
};
