#include "AIQueryDispatcher.h"
#include "nlohmann/json.hpp"

#include "httplib.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
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
    NarrationTriggerConfig trigger,
    int window_size,
    DeepSeekConfig deepseek,
    bool enabled,
    int ollama_timeout_s,
    int ollama_max_tokens,
    double ollama_temperature)
    : storage_(storage), ollama_url_(std::move(ollama_url)), model_(std::move(model)), trigger_(std::move(trigger)), window_size_(window_size), deepseek_(std::move(deepseek)), enabled_(enabled), ollama_timeout_s_(ollama_timeout_s), ollama_max_tokens_(ollama_max_tokens), ollama_temperature_(ollama_temperature)
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
// 只评估触发条件和入队，推理全部交给 worker，不阻塞 HTTP 请求
// ─────────────────────────────────────────────
void AIQueryDispatcher::onNewData(const std::vector<SensorReading> &readings,
                                  bool anomaly_detected)
{
    if (!enabled_ || readings.empty())
        return;

    const std::string &device_id = readings[0].device_id;

    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 稳态返回空串 —— 不入队，也就不会有任何 LLM 调用
        const std::string reason = evaluateTrigger(readings, anomaly_detected);
        if (reason.empty())
            return;

        // 同一设备在队列中只保留一份（worker 取到时会读最新窗口）
        if (pending_set_.insert(device_id).second)
        {
            pending_.emplace_back(device_id, reason);
            should_notify = true;
        }
    }

    if (should_notify)
        cv_.notify_one();
}

// ─────────────────────────────────────────────
// 触发条件评估（持锁调用）
//
// 任一成立即触发：
//   1. 首次上报          → 给出基线叙述
//   2. DataFilter 判异常 → 确定性层已经认定这批数据不正常
//   3. 跨越告警带边界    → 进入或离开都算状态变化
//   4. 较上次叙述明显漂移
// 除首次和异常外，都受 min_interval_s 冷却限制。
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::evaluateTrigger(
    const std::vector<SensorReading> &readings,
    bool anomaly_detected)
{
    const std::string &device_id = readings[0].device_id;
    auto &st = states_[device_id];
    const auto now = std::chrono::steady_clock::now();

    // 无论是否触发，告警带状态都要先算出来（用于跨越检测）
    std::string reason;

    if (!st.ever_narrated)
    {
        reason = "first report from this device (baseline)";
    }
    else if (anomaly_detected)
    {
        reason = "IQR anomaly detector flagged this batch";
    }

    // 跨越告警带：状态的定性变化，不受冷却限制。
    // 之所以安全，是因为带内状态被锁存 —— 只在“进/出”那一刻触发，
    // 持续高温不会反复触发（这点和 IQR 异常不同）。
    // 迟滞：贴着边界抖动时不会来回触发。
    bool band_crossed = false;
    for (const auto &r : readings)
    {
        double band = 0.0;
        if (!warnBandFor(r.sensor_type, band))
            continue;

        double drift_threshold = 0.0;
        driftThresholdFor(r.sensor_type, drift_threshold);
        const double hysteresis = drift_threshold * 0.5;

        auto it = st.in_warn_band.find(r.sensor_type);
        const bool was_inside = it != st.in_warn_band.end() && it->second;

        // 进入要求明确越过边界，退出要求明确低于边界
        const bool now_inside = was_inside ? (r.value > band - hysteresis)
                                           : (r.value > band);

        if (st.ever_narrated && now_inside != was_inside)
        {
            std::ostringstream os;
            os << r.sensor_type << (now_inside ? " rose above " : " fell below ")
               << std::fixed << std::setprecision(1) << band << " " << r.unit;
            reason = os.str();
            band_crossed = true;
        }
        st.in_warn_band[r.sensor_type] = now_inside;
    }

    // 漂移：相对上次叙述时的值，不是相对上一条读数
    if (reason.empty() && st.ever_narrated)
    {
        for (const auto &r : readings)
        {
            double threshold = 0.0;
            if (!driftThresholdFor(r.sensor_type, threshold))
                continue;

            auto it = st.last_value.find(r.sensor_type);
            if (it == st.last_value.end())
                continue;

            const double drift = std::fabs(r.value - it->second);
            if (drift >= threshold)
            {
                std::ostringstream os;
                os << r.sensor_type << " drifted " << std::fixed << std::setprecision(1)
                   << drift << " " << r.unit << " since the last narration";
                reason = os.str();
                break;
            }
        }
    }

    if (reason.empty())
        return ""; // 稳态：保持沉默

    // 冷却：首次上报和跨越告警带不受限（两者都不会连发）。
    //
    // 异常必须受冷却限制：水平发生持续位移后，IQR 会把之后每一条都判为离群，
    // 直到滑动窗口被新值填满为止 —— 绕过冷却会让这段时间变成叙述刷屏。
    if (st.ever_narrated && !band_crossed &&
        st.last_time.time_since_epoch().count() != 0)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - st.last_time)
                                 .count();
        if (elapsed < trigger_.min_interval_s)
            return "";
    }

    // 更新基线（漂移从这一刻重新计算）
    for (const auto &r : readings)
        st.last_value[r.sensor_type] = r.value;
    st.last_time = now;
    st.ever_narrated = true;

    return reason;
}

// 只有已知量纲的类型参与漂移触发；未知 sensor_type 不猜阈值。
bool AIQueryDispatcher::driftThresholdFor(const std::string &sensor_type,
                                          double &out) const
{
    if (sensor_type == "temperature")
        out = trigger_.temp_delta_c;
    else if (sensor_type == "humidity")
        out = trigger_.humidity_delta;
    else
        return false;
    return true;
}

bool AIQueryDispatcher::warnBandFor(const std::string &sensor_type,
                                    double &out) const
{
    if (sensor_type == "temperature")
        out = trigger_.warn_temp_c;
    else if (sensor_type == "humidity")
        out = trigger_.warn_humidity;
    else
        return false;
    return true;
}

// ─────────────────────────────────────────────
// worker 主循环：串行消费待分析设备
// ─────────────────────────────────────────────
void AIQueryDispatcher::workerLoop()
{
    while (true)
    {
        std::string device_id;
        std::string reason;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]
                     { return stop_ || !pending_.empty(); });

            if (stop_)
                return;

            device_id = pending_.front().first;
            reason = pending_.front().second;
            pending_.pop_front();
            pending_set_.erase(device_id);
        }

        try
        {
            runAnalysis(device_id, reason);
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
void AIQueryDispatcher::runAnalysis(const std::string &device_id,
                                    const std::string &reason)
{
    std::cout << "[AIQuery] Narrating " << device_id
              << " — reason: " << reason << std::endl;

    // 1. 查询窗口数据（getRecentReadings 按 timestamp DESC，最新在前）
    auto history = storage_.getRecentReadings(device_id, window_size_);

    if (history.empty())
    {
        std::cerr << "[AIQuery] No history, skip." << std::endl;
        return;
    }

    // 2. 聚合为状态摘要 — 模型看到的是统计量，不是原始数据行
    const auto summaries = summarize(history);

    // 3. 构建 prompt → 调用 LLM（DeepSeek 优先，失败降级 Ollama）
    const std::string prompt = buildPrompt(device_id, summaries, reason);
    const std::string result = callLLM(prompt);

    // 4. 存储叙述结果
    saveResult(device_id, prompt, result);
}

// ─────────────────────────────────────────────
// 窗口数据 → 按 sensor_type 聚合统计
// ─────────────────────────────────────────────
std::vector<AIQueryDispatcher::SensorSummary> AIQueryDispatcher::summarize(
    const std::vector<SensorReading> &history)
{
    // 保持首次出现顺序，输出稳定，便于人肉比对
    std::vector<SensorSummary> out;
    std::unordered_map<std::string, size_t> index;

    for (const auto &r : history)
    {
        auto it = index.find(r.sensor_type);
        if (it == index.end())
        {
            SensorSummary s;
            s.sensor_type = r.sensor_type;
            s.unit = r.unit;
            s.count = 1;
            s.min = s.max = s.mean = r.value;
            // history 是 DESC，首个遇到的即窗口内最新一条
            s.latest = r.value;
            s.latest_ts = r.server_timestamp;
            s.oldest = r.value;
            s.oldest_ts = r.server_timestamp;
            index[r.sensor_type] = out.size();
            out.push_back(std::move(s));
            continue;
        }

        SensorSummary &s = out[it->second];
        s.count += 1;
        s.min = std::min(s.min, r.value);
        s.max = std::max(s.max, r.value);
        s.mean += (r.value - s.mean) / s.count; // 增量均值，避免累加溢出
        // 越往后越旧
        s.oldest = r.value;
        s.oldest_ts = r.server_timestamp;
    }

    for (auto &s : out)
    {
        s.delta = s.latest - s.oldest;

        // 变化率是关键：没有它，模型分不清“两小时升 8 度”和“八秒升 8 度”
        int64_t t_old = 0, t_new = 0;
        if (parseIso8601(s.oldest_ts, t_old) && parseIso8601(s.latest_ts, t_new))
        {
            s.span_seconds = static_cast<double>(t_new - t_old);
            if (s.span_seconds > 0.0)
            {
                s.rate_per_min = s.delta * 60.0 / s.span_seconds;
                s.rate_valid = true;
            }
        }
    }

    return out;
}

// 手动解析，避免依赖 macOS 上支持不全的 <chrono> from_chars（与 DataFilter 一致）
bool AIQueryDispatcher::parseIso8601(const std::string &ts, int64_t &out)
{
    if (ts.size() < 19)
        return false;
    try
    {
        struct tm t = {};
        t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
        t.tm_mon = std::stoi(ts.substr(5, 2)) - 1;
        t.tm_mday = std::stoi(ts.substr(8, 2));
        t.tm_hour = std::stoi(ts.substr(11, 2));
        t.tm_min = std::stoi(ts.substr(14, 2));
        t.tm_sec = std::stoi(ts.substr(17, 2));
        out = timegm(&t);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// ─────────────────────────────────────────────
// 构建 Prompt（状态摘要 → 人话叙述）
// ─────────────────────────────────────────────
std::string AIQueryDispatcher::buildPrompt(
    const std::string &device_id,
    const std::vector<SensorSummary> &summaries,
    const std::string &reason)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    // 角色边界写进 prompt：模型只解释，不告警、不控制。
    // 这样它既不会输出越权的“立即报警”，也不必为了安全兜底而夸大措辞。
    ss << "You explain environmental sensor data for an IoT monitoring system.\n"
       << "You do NOT control alarms. Alarms are handled deterministically and have\n"
       << "already acted before you see this: the device applies local thresholds and\n"
       << "sounds its own buzzer, and the server runs IQR outlier detection.\n"
       << "Your only job is to tell a human what is going on, in plain language.\n\n";

    ss << "Device: " << device_id << "\n"
       << "You were invoked because: " << reason << "\n"
       << "(This system stays silent while nothing changes, so something did change.)\n\n";

    ss << "## Current state (aggregated over the recent window)\n";
    for (const auto &s : summaries)
    {
        ss << "- " << s.sensor_type << " [" << s.unit << "]"
           << "  now=" << s.latest
           << "  min=" << s.min
           << "  max=" << s.max
           << "  mean=" << s.mean
           << "  change=" << (s.delta >= 0 ? "+" : "") << s.delta
           << "  samples=" << s.count << "\n"
           << "    over " << s.span_seconds << " s";
        if (s.rate_valid)
            ss << "  = " << (s.rate_per_min >= 0 ? "+" : "") << s.rate_per_min
               << " " << s.unit << "/min";
        else
            ss << "  (span too short to compute a rate)";
        ss << "\n";
    }

    // 明确告诉模型系统已有哪些能力，否则它会建议“加个滤波”——而固件早就有中值滤波了
    ss << "\n## Already implemented (do NOT recommend these)\n"
       << "- The device median-filters every reading before sending it.\n"
       << "- The server runs IQR outlier detection over a sliding window.\n"
       << "- Readings are range-validated before storage.\n"
       << "- A local buzzer alarm already fires on threshold breach.\n\n";

    ss << "## How to answer\n"
       << "- The invocation reason above is an established fact from deterministic code,\n"
       << "  not a hypothesis. Never contradict it or claim nothing was detected.\n"
       << "- Judge magnitude by the RATE, not the raw change. A large change over a long\n"
       << "  span is routine; the same change over seconds is not physically plausible\n"
       << "  for room air and usually means the sensor was disturbed directly.\n"
       << "- Quote the actual numbers above. A summary without numbers is useless.\n"
       << "- Commit to the single most likely reading of the data. Do NOT list several\n"
       << "  causes as equally plausible. If alternatives genuinely matter, name the one\n"
       << "  observation that would tell them apart.\n"
       << "- Do NOT manufacture concern. If this is benign or routine, say so plainly.\n"
       << "- Do NOT speculate about sensor failure unless the numbers themselves show it\n"
       << "  (impossible values, physically inconsistent combinations).\n"
       << "- severity describes the situation for a human reader; it triggers nothing.\n\n";

    ss << "Return only JSON, no Markdown. ASCII only - write \"C\", never the degree sign:\n"
       << "{\n"
       << "  \"situation\": \"one sentence: what is happening, with numbers\",\n"
       << "  \"severity\": \"normal | watch | concern | urgent\",\n"
       << "  \"explanation\": \"2-3 sentences: why the data looks this way\",\n"
       << "  \"suggested_action\": \"what a person should do, or 'none'\",\n"
       << "  \"verdict\": \"max 20 chars, for a small display\"\n"
       << "}";

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

std::string AIQueryDispatcher::answerPrompt(const std::string &device_id,
                                            const std::string &user_prompt)
{
    auto history = storage_.getRecentReadings(device_id, window_size_);
    const auto summaries = summarize(history);

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    // 这条回答只进浏览器，不进 OLED —— 屏幕上的文字走的是独立的
    // oled: 命令通道，那里才有 ASCII 限制。所以这里直接输出中文，
    // 不必先英文再由前端做词典替换（那会得到半中半英的句子）。
    ss << "你是一个物联网环境监测助手。\n"
       << "根据下面的传感器数据回答用户的问题。\n"
       << "用中文回答，1-3 句话，简洁、以数据为依据。\n"
       << "引用具体数值，不要编造数据里没有的信息。\n\n";

    ss << "Device: " << device_id << "\n\n";

    if (!summaries.empty())
    {
        ss << "## Recent sensor data\n";
        for (const auto &s : summaries)
        {
            ss << "- " << s.sensor_type << " [" << s.unit << "]"
               << "  now=" << s.latest
               << "  min=" << s.min << "  max=" << s.max
               << "  mean=" << s.mean
               << "  change=" << (s.delta >= 0 ? "+" : "") << s.delta
               << "  over " << s.span_seconds << "s"
               << "  samples=" << s.count << "\n";
        }
    }
    else
    {
        ss << "No sensor data available.\n";
    }

    ss << "\nUser question: " << user_prompt << "\n";

    return callLLM(ss.str());
}
