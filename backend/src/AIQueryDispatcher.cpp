#include "AIQueryDispatcher.h"
#include "nlohmann/json.hpp"

#include "httplib.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

// ─────────────────────────────────────────────
// 构造函数（依赖注入）— 同时启动后台 worker 线程
// ─────────────────────────────────────────────
AIQueryDispatcher::AIQueryDispatcher(
    StorageEngine &storage,
    std::string ollama_url,
    std::string model,
    int trigger_count,
    int window_size,
    DeepSeekConfig deepseek,
    bool enabled,
    int ollama_timeout_s,
    int ollama_max_tokens,
    double ollama_temperature)
    : storage_(storage), ollama_url_(std::move(ollama_url)), model_(std::move(model)), trigger_count_(trigger_count), window_size_(window_size), deepseek_(std::move(deepseek)), enabled_(enabled), ollama_timeout_s_(ollama_timeout_s), ollama_max_tokens_(ollama_max_tokens), ollama_temperature_(ollama_temperature)
{
    worker_ = std::thread(&AIQueryDispatcher::workerLoop, this);
}

// ─────────────────────────────────────────────
// 析构：通知并回收 worker 线程
// ─────────────────────────────────────────────
AIQueryDispatcher::~AIQueryDispatcher()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

// ─────────────────────────────────────────────
// 数据入口（由 DataIngestor 的请求线程调用）
// 只做计数和入队，推理全部交给 worker，不阻塞 HTTP 请求
// ─────────────────────────────────────────────
void AIQueryDispatcher::onNewData(const std::vector<SensorReading> &readings)
{
    /**
     * 设计说明：
     * - 每个 device 独立计数
     * - readings 可能包含多个传感器数据
     * - 按“记录条数”计数（更通用）
     */
    if (!enabled_ || readings.empty())
        return;

    const std::string &device_id = readings[0].device_id;

    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 累加条数
        counters_[device_id] += static_cast<int>(readings.size());

        // 达到阈值则交给 worker 分析（同一设备在队列中只保留一份）
        if (counters_[device_id] >= trigger_count_)
        {
            counters_[device_id] = 0;

            if (pending_set_.insert(device_id).second)
            {
                pending_.push_back(device_id);
                should_notify = true;
            }
        }
    }

    if (should_notify)
        cv_.notify_one();
}

// ─────────────────────────────────────────────
// worker 主循环：串行消费待分析设备
// ─────────────────────────────────────────────
void AIQueryDispatcher::workerLoop()
{
    while (true)
    {
        std::string device_id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]
                     { return stop_ || !pending_.empty(); });

            if (stop_)
                return;

            device_id = pending_.front();
            pending_.pop_front();
            pending_set_.erase(device_id);
        }

        try
        {
            runAnalysis(device_id);
        }
        catch (const std::exception &e)
        {
            std::cerr << "[AIQuery] Analysis failed: "
                      << e.what()
                      << std::endl;
        }
    }
}

// ─────────────────────────────────────────────
// 执行完整分析流程（worker 线程）
// ─────────────────────────────────────────────
void AIQueryDispatcher::runAnalysis(const std::string &device_id)
{
    std::cout << "[AIQuery] Triggered for device: "
              << device_id << std::endl;

    /**
     * 1. 查询历史数据
     * - 从数据库获取最近 window_size 条
     * - 数据包含多种 sensor_type（统一模型）
     */
    auto history = storage_.getRecentReadings(device_id, window_size_);

    if (history.empty())
    {
        std::cerr << "[AIQuery] No history, skip." << std::endl;
        return;
    }

    /**
     * 2. 构建 Prompt
     * - 将历史数据序列化为文本
     * - 交由 LLM 进行语义分析
     */
    std::string prompt = buildPrompt(device_id, history);

    /**
     * 3. 调用 LLM（DeepSeek 优先，失败降级 Ollama）
     */
    std::string result = callLLM(prompt);

    /**
     * 4. 存储分析结果
     */
    saveResult(device_id, prompt, result);
    std::cout << "[AIQuery] Result: "
              << result.substr(0, 80) << "..."
              << std::endl;
}

// ─────────────────────────────────────────────
// 构建 Prompt（核心AI输入）
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::buildPrompt(
    const std::string &device_id,
    const std::vector<SensorReading> &history)
{
    std::ostringstream ss;

    /**
     * 设计说明：
     * - 不再区分 temp/humi
     * - 统一格式 → 更易扩展
     * - 让模型自己理解 sensor_type
     */
    ss << "You are an IoT sensor data analyst.\n";
    ss << "Analyze the recent readings from device ["
       << device_id << "] and provide insights.\n\n";

    ss << "## Sensor Readings\n";

    for (const auto &r : history)
    {
        ss << r.server_timestamp << "  "
           << r.sensor_type << "  "
           << r.value << " " << r.unit << "\n";
    }

    ss << "\nAnswer in 2-3 sentences:\n"
       << "1. Is the system stable?\n"
       << "2. Any anomaly or trend?\n"
       << "3. Suggestions?\n"
       << "Be concise.";

    return ss.str();
}

// ─────────────────────────────────────────────
// 统一推理入口：DeepSeek 主，Ollama 备
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::callLLM(const std::string &prompt)
{
    if (deepseek_.enabled && !deepseek_.api_key.empty())
    {
        try
        {
            std::string result = callDeepSeek(prompt);
            std::cout << "[AIQuery] backend=deepseek model="
                      << deepseek_.model << std::endl;
            return result;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[AIQuery] DeepSeek failed (" << e.what()
                      << "), falling back to local Ollama." << std::endl;
        }
    }

    std::string result = callOllama(prompt);
    std::cout << "[AIQuery] backend=ollama model=" << model_ << std::endl;
    return result;
}

// ─────────────────────────────────────────────
// 调用云端 DeepSeek（OpenAI 兼容 API）
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::callDeepSeek(const std::string &prompt)
{
    // base_url 形如 "https://api.deepseek.com"，httplib 通用 Client 自行解析 scheme
    httplib::Client cli(deepseek_.base_url);

    cli.set_connection_timeout(5);
    cli.set_read_timeout(deepseek_.timeout_s);
    cli.set_default_headers({{"Authorization", "Bearer " + deepseek_.api_key}});

    json req = {
        {"model", deepseek_.model},
        {"messages", json::array({json{{"role", "user"}, {"content", prompt}}})},
        {"stream", false}};

    auto res = cli.Post("/chat/completions",
                        req.dump(),
                        "application/json");

    if (!res)
    {
        throw std::runtime_error("DeepSeek connection failed: " +
                                 httplib::to_string(res.error()));
    }

    if (res->status != 200)
    {
        throw std::runtime_error("DeepSeek HTTP " +
                                 std::to_string(res->status));
    }

    auto resp = json::parse(res->body);

    return resp.at("choices").at(0).at("message").at("content").get<std::string>();
}

// ─────────────────────────────────────────────
// 调用 Ollama REST API（本地备用）
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::callOllama(const std::string &prompt)
{
    /**
     * URL解析
     * 默认：http://localhost:11434
     */
    std::string host = "localhost";
    int port = 11434;

    std::string url = ollama_url_;
    if (url.substr(0, 7) == "http://")
    {
        url = url.substr(7);
    }

    auto pos = url.find(':');
    if (pos != std::string::npos)
    {
        host = url.substr(0, pos);
        port = std::stoi(url.substr(pos + 1));
    }

    httplib::Client cli(host, port);

    // 网络参数（超时可配置：config ollama.timeout_s）
    cli.set_connection_timeout(5);
    cli.set_read_timeout(ollama_timeout_s_);

    /**
     * 构建请求体（max_tokens / temperature 可配置）
     */
    json req = {
        {"model", model_},
        {"prompt", prompt},
        {"stream", false},
        {"options", {{"num_predict", ollama_max_tokens_}, {"temperature", ollama_temperature_}}}};

    auto res = cli.Post("/api/generate",
                        req.dump(),
                        "application/json");

    // 网络错误
    if (!res)
    {
        throw std::runtime_error("Ollama connection failed");
    }

    // HTTP错误
    if (res->status != 200)
    {
        throw std::runtime_error("Ollama HTTP " +
                                 std::to_string(res->status));
    }

    // JSON解析
    auto resp = json::parse(res->body);

    return resp.at("response").get<std::string>();
}

// ─────────────────────────────────────────────
// 保存分析结果（worker 线程；缓存更新需加锁）
// ─────────────────────────────────────────────
void AIQueryDispatcher::saveResult(
    const std::string &device_id,
    const std::string &prompt,
    const std::string &result)
{
    std::cout << "[AIQuery] === RESULT ===\n"
              << result << "\n"
              << "[AIQuery] ==============\n";

    storage_.insertAnalysisLog(device_id, prompt, result);

    // 缓存最新结果，供 DataIngestor 回传给设备端
    std::lock_guard<std::mutex> lock(mutex_);
    last_analysis_[device_id] = result;
}

// ─────────────────────────────────────────────
// 获取最近一次分析（请求线程；加锁返回拷贝）
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::getLastAnalysis(const std::string &device_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_analysis_.find(device_id);
    if (it != last_analysis_.end())
        return it->second;
    return "";
}
