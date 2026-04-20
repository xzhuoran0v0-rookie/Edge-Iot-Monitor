#include "AIQueryDispatcher.h"
#include "nlohmann/json.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "httplib.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

// ─────────────────────────────────────────────
// 构造函数（依赖注入）
// ─────────────────────────────────────────────
AIQueryDispatcher::AIQueryDispatcher(
    StorageEngine &storage,
    std::string ollama_url,
    std::string model,
    int trigger_count,
    int window_size)
    : storage_(storage), ollama_url_(std::move(ollama_url)), model_(std::move(model)), trigger_count_(trigger_count), window_size_(window_size)
{
}

// ─────────────────────────────────────────────
// 数据入口（由 DataIngestor 调用）
// ─────────────────────────────────────────────
void AIQueryDispatcher::onNewData(const std::vector<SensorReading> &readings)
{
    /**
     * 设计说明：
     * - 每个 device 独立计数
     * - readings 可能包含多个传感器数据
     * - 按“记录条数”计数（更通用）
     */
    if (readings.empty())
        return;

    const std::string &device_id = readings[0].device_id;

    // 累加条数
    counters_[device_id] += static_cast<int>(readings.size());

    // 达到阈值则触发分析
    if (counters_[device_id] >= trigger_count_)
    {
        counters_[device_id] = 0;

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
// 执行完整分析流程
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
     * 3.调用Ollama
     */
    std::string result = callOllama(prompt);

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
// 调用 Ollama REST API
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

    // 网络参数
    cli.set_connection_timeout(5);
    cli.set_read_timeout(60);

    /**
     * 构建请求体
     */
    json req = {
        {"model", model_},
        {"prompt", prompt},
        {"stream", false}};

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
// 保存分析结果
// ─────────────────────────────────────────────
void AIQueryDispatcher::saveResult(
    const std::string &device_id,
    const std::string &prompt,
    const std::string &result)
{
    /**
     * 当前实现：占位（打印）
     * 推荐：调用 StorageEngine::insertAnalysisLog()
     */

    std::cout << "[AIQuery] === RESULT ===\n"
              << result << "\n"
              << "[AIQuery] ==============\n";

    // TODO:
    // storage_.insertAnalysisLog(device_id, prompt, result);
}