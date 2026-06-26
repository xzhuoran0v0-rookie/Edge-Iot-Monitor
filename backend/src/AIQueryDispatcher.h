#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "StorageEngine.h"

/**
 * @brief 云端 DeepSeek 推理配置（OpenAI 兼容 /chat/completions）
 *
 * enabled 且 api_key 非空时作为首选推理后端；
 * 调用失败（网络/超时/非200/解析错误）自动降级到本地 Ollama。
 */
struct DeepSeekConfig
{
    bool enabled = false;
    std::string base_url = "https://api.deepseek.com";
    std::string model = "deepseek-chat";
    std::string api_key;
    int timeout_s = 30;
};

/**
 * @brief AI分析调度模块
 *
 * 推理后端：
 * - 首选：云端 DeepSeek（OpenAI 兼容 API）
 * - 备用：本地 Ollama（DeepSeek 未启用或调用失败时降级）
 *
 * 功能：
 * - 接收来自 DataIngestor 的新数据（统一为 vector<SensorReading>）
 * - 按 device_id 统计数据条数
 * - 达到 trigger_count 后触发一次 AI 分析
 * - 从 StorageEngine 查询历史数据
 * - 构建 prompt → 调用 LLM → 存入 analysis_log
 *
 * 线程模型：
 * - onNewData/getLastAnalysis 由 httplib 请求线程并发调用，
 *   counters_/last_analysis_/pending_ 由 mutex_ 保护
 * - 推理在单个后台 worker 线程执行，HTTP 请求线程只入队不阻塞
 */
class AIQueryDispatcher
{
public:
    /**
     * @brief 构造函数
     *
     * @param storage 数据存储接口(依赖注入)
     * @param ollama_url ollama服务器地址（本地备用后端）
     * @param model ollama 模型名称
     * @param trigger_conut 每累计多少条数据触发一次分析
     * @param window_size 每次分析使用的历史数据条数
     * @param deepseek 云端 DeepSeek 配置（首选后端）
     * @param enabled 总开关（config ai.enabled；false 时 onNewData 直接返回）
     * @param ollama_timeout_s Ollama 读取超时（秒）
     * @param ollama_max_tokens Ollama 最大输出 token 数
     * @param ollama_temperature Ollama 采样温度
     */
    explicit AIQueryDispatcher(
        StorageEngine &storage,
        std::string ollama_url = "http://localhost:11434",
        std::string model = "qwen2.5:3b",
        int trigger_count = 20,
        int window_size = 10,
        DeepSeekConfig deepseek = {},
        bool enabled = true,
        int ollama_timeout_s = 60,
        int ollama_max_tokens = 512,
        double ollama_temperature = 0.3);

    /** 停止并回收后台 worker 线程 */
    ~AIQueryDispatcher();

    AIQueryDispatcher(const AIQueryDispatcher &) = delete;
    AIQueryDispatcher &operator=(const AIQueryDispatcher &) = delete;

    /**
     * @brief 接收新数据（由 DataIngestor 调用，线程安全、不阻塞）
     *
     * 行为：
     * - 按 device_id 累加数据条数
     * - 当累计达到 trigger_count 时把 device_id 交给后台 worker 分析
     *
     * @param readings 新入库的数据（可能包含多个传感器）
     */
    void onNewData(const std::vector<SensorReading> &readings);

    /**
     * @brief 获取最近一次 AI 分析结果（用于回传给设备端，线程安全）
     *
     * @param device_id 设备ID
     * @return 最近一次分析文本；如果还没跑过 AI 则返回空字符串 ""
     */
    std::string getLastAnalysis(const std::string &device_id) const;

private:
    /**
     * @brief 构建发送给 LLM 的 prompt
     *
     * @param device_id 设备ID
     * @param history   历史数据（最近 window_size 条）
     * @return prompt字符串
     */
    std::string buildPrompt(const std::string &device_id,
                            const std::vector<SensorReading> &history);

    /**
     * @brief 统一推理入口：DeepSeek 优先，失败降级 Ollama
     *
     * @param prompt 输入提示词
     * @return 模型输出文本
     */
    std::string callLLM(const std::string &prompt);

    /**
     * @brief 调用云端 DeepSeek（OpenAI 兼容 /chat/completions）
     *
     * @param prompt 输入提示词
     * @return 模型输出文本
     */
    std::string callDeepSeek(const std::string &prompt);

    /**
     * @brief 调用本地 Ollama 推理接口（POST /api/generate）
     *
     * @param prompt 输入提示词
     * @return 模型输出文本
     */
    std::string callOllama(const std::string &prompt);

    /**
     * @brief 将AI分析结果写入数据库并更新缓存
     */
    void saveResult(const std::string &device_id,
                    const std::string &prompt,
                    const std::string &result);

    /**
     * @brief 执行一次完整分析流程（在 worker 线程中运行）
     *
     * 流程：
     *   1. 查询历史数据
     *   2. 构建 prompt
     *   3. 调用 LLM（DeepSeek → Ollama 降级）
     *   4. 写入 analysis_log
     */
    void runAnalysis(const std::string &device_id);

    /** 后台 worker 主循环：从 pending_ 取 device_id 执行 runAnalysis */
    void workerLoop();

private:
    StorageEngine &storage_; ///< 数据存储接口（不使用 friend）

    std::string ollama_url_; ///< Ollama服务地址（备用后端）
    std::string model_;      ///< Ollama 模型名称

    int trigger_count_; ///< 触发分析阈值（数据条数）
    int window_size_;   ///< 分析窗口大小

    DeepSeekConfig deepseek_; ///< 云端首选后端配置
    bool enabled_;            ///< 总开关（ai.enabled）

    int ollama_timeout_s_;      ///< Ollama 读取超时（秒）
    int ollama_max_tokens_;     ///< Ollama 最大输出 token
    double ollama_temperature_; ///< Ollama 采样温度

    /**
     * @brief 每个设备的计数器
     *
     * key   = device_id
     * value = 自上次分析后累计的数据条数
     */
    std::unordered_map<std::string, int> counters_;

    /** 最近一次 AI 分析结果缓存（key = device_id） */
    std::unordered_map<std::string, std::string> last_analysis_;

    // ---- worker 线程状态（mutex_ 保护 counters_/last_analysis_/pending_/stop_） ----
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> pending_;             ///< 待分析 device_id 队列
    std::unordered_set<std::string> pending_set_; ///< 队列去重
    bool stop_ = false;
    std::thread worker_;
};
