#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
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
 * @brief 叙述触发条件（state-change trigger）
 *
 * 旧实现按“每累计 N 条数据触发一次”，环境稳定时会反复分析同一批数据 —
 * 模型只能重复描述输入，产出没有信息量，还持续消耗 API 配额。
 * 现在只在“状态发生变化”时触发：异常、跨越告警带、或较上次叙述明显漂移。
 */
struct NarrationTriggerConfig
{
    int min_interval_s = 60;     ///< 同一设备两次叙述的最小间隔（防抖 + 控制成本）
    double temp_delta_c = 2.0;   ///< 温度较上次叙述漂移超过该值即触发
    double humidity_delta = 5.0; ///< 湿度漂移阈值（%RH）
    double pressure_delta = 5.0; ///< 气压漂移阈值（hPa）
    double warn_temp_c = 35.0;   ///< 温度告警带边界（进入或离开都算状态变化）
    double warn_humidity = 80.0; ///< 湿度告警带边界（%RH）
};

/**
 * @brief AI 叙述调度模块
 *
 * 定位（重要）：
 * - 判定由确定性代码负责 — DataFilter 的 IQR、阈值、固件本地逻辑。
 * - 本模块只负责“把状态翻译成人话”，不参与任何告警或控制决策。
 *   模型答错的代价是一句啰嗦的话，而不是一次漏报。
 *
 * 推理后端：
 * - 首选：云端 DeepSeek（OpenAI 兼容 API）
 * - 备用：本地 Ollama（DeepSeek 未启用或调用失败时降级）
 *
 * 流程：
 * - 接收来自 DataIngestor 的新数据 + 本批是否被判定为异常
 * - 评估状态变化触发条件（见 NarrationTriggerConfig），稳态保持沉默
 * - 触发后从 StorageEngine 取窗口数据 → 汇总为状态摘要 → 调用 LLM → 写 analysis_log
 *
 * 线程模型：
 * - onNewData/getLastAnalysis 由 httplib 请求线程并发调用，
 *   states_/last_analysis_/pending_ 由 mutex_ 保护
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
     * @param trigger 状态变化触发条件（取代旧的按条数计数）
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
        NarrationTriggerConfig trigger = {},
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
     * - 评估状态变化触发条件（异常 / 跨越告警带 / 明显漂移 / 首次上报）
     * - 稳态下不触发，直接返回，不产生任何 LLM 调用
     * - 触发时把 device_id + 触发原因交给后台 worker
     *
     * @param readings 新入库的数据（可能包含多个传感器）
     * @param anomaly_detected 本批是否被 DataFilter 判定为异常（确定性判定结果）
     */
    void onNewData(const std::vector<SensorReading> &readings,
                   bool anomaly_detected = false);

    /**
     * @brief 获取最近一次 AI 分析结果（用于回传给设备端，线程安全）
     *
     * @param device_id 设备ID
     * @return 最近一次分析文本；如果还没跑过 AI 则返回空字符串 ""
     */
    std::string getLastAnalysis(const std::string &device_id) const;

private:
    /**
     * @brief 单个 sensor_type 在窗口内的统计摘要
     *
     * 送给模型的是这个，而不是原始数据行 —— 给模型一堆行，它只能把行复述回来。
     */
    struct SensorSummary
    {
        std::string sensor_type;
        std::string unit;
        int count = 0;
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double oldest = 0.0; ///< 窗口内最早一条的值
        double latest = 0.0; ///< 窗口内最新一条的值
        double delta = 0.0;  ///< latest - oldest
        double span_seconds = 0.0; ///< 窗口时间跨度
        double rate_per_min = 0.0; ///< delta / 分钟；没有变化率模型无法区分缓慢漂移和骤变
        bool rate_valid = false;   ///< 跨度为 0（同秒内）时变化率无意义
        std::string oldest_ts;
        std::string latest_ts;
    };

    /** ISO8601（...Z）→ epoch 秒；解析失败返回 false */
    static bool parseIso8601(const std::string &ts, int64_t &out);

    /**
     * @brief 把窗口数据按 sensor_type 聚合成统计摘要
     *
     * @param history getRecentReadings 的结果（注意：按 timestamp DESC，最新在前）
     */
    static std::vector<SensorSummary> summarize(
        const std::vector<SensorReading> &history);

    /**
     * @brief 构建发送给 LLM 的 prompt（状态摘要，非原始数据行）
     *
     * @param device_id 设备ID
     * @param summaries 窗口统计摘要
     * @param reason    本次触发原因（写进 prompt，让模型知道该解释什么）
     * @return prompt字符串
     */
    std::string buildPrompt(const std::string &device_id,
                            const std::vector<SensorSummary> &summaries,
                            const std::string &reason);

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
     * @brief 执行一次完整叙述流程（在 worker 线程中运行）
     *
     * 流程：
     *   1. 查询窗口数据
     *   2. 聚合为状态摘要
     *   3. 构建 prompt → 调用 LLM（DeepSeek → Ollama 降级）
     *   4. 写入 analysis_log
     */
    void runAnalysis(const std::string &device_id, const std::string &reason);

    /** 后台 worker 主循环：从 pending_ 取任务执行 runAnalysis */
    void workerLoop();

    /**
     * @brief 评估是否需要叙述，并更新设备状态基线
     *
     * @param readings 本批数据
     * @param anomaly_detected DataFilter 的判定结果
     * @return 触发原因；返回空字符串表示稳态、不触发
     *
     * @note 必须在持有 mutex_ 的情况下调用（读写 states_）。
     */
    std::string evaluateTrigger(const std::vector<SensorReading> &readings,
                                bool anomaly_detected);

    /** 取某个 sensor_type 的漂移阈值；返回 false 表示该类型不参与漂移触发 */
    bool driftThresholdFor(const std::string &sensor_type, double &out) const;

    /** 取某个 sensor_type 的告警带边界；返回 false 表示该类型没有告警带 */
    bool warnBandFor(const std::string &sensor_type, double &out) const;

private:
    StorageEngine &storage_; ///< 数据存储接口（不使用 friend）

    std::string ollama_url_; ///< Ollama服务地址（备用后端）
    std::string model_;      ///< Ollama 模型名称

    NarrationTriggerConfig trigger_; ///< 状态变化触发条件
    int window_size_;                ///< 分析窗口大小

    DeepSeekConfig deepseek_; ///< 云端首选后端配置
    bool enabled_;            ///< 总开关（ai.enabled）

    int ollama_timeout_s_;      ///< Ollama 读取超时（秒）
    int ollama_max_tokens_;     ///< Ollama 最大输出 token
    double ollama_temperature_; ///< Ollama 采样温度

    /**
     * @brief 每个设备的叙述基线
     *
     * 漂移是相对“上次叙述时的值”而不是上一条读数 —— 否则缓慢升温永远
     * 每步都低于阈值，累积再大也触发不了。
     */
    struct DeviceNarrationState
    {
        std::unordered_map<std::string, double> last_value; ///< 上次叙述时各 sensor_type 的值
        std::unordered_map<std::string, bool> in_warn_band; ///< 上次是否在告警带内（检测跨越）
        std::chrono::steady_clock::time_point last_time{};
        bool ever_narrated = false; ///< 首次上报总是叙述一次，给出基线
    };

    /** 每设备叙述基线（key = device_id） */
    std::unordered_map<std::string, DeviceNarrationState> states_;

    /** 最近一次 AI 分析结果缓存（key = device_id） */
    std::unordered_map<std::string, std::string> last_analysis_;

    // ---- worker 线程状态（mutex_ 保护 states_/last_analysis_/pending_/stop_） ----
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    /** 待叙述队列：<device_id, 触发原因> */
    std::deque<std::pair<std::string, std::string>> pending_;
    std::unordered_set<std::string> pending_set_; ///< 按 device_id 去重
    bool stop_ = false;
    std::thread worker_;
};
