#include "DataIngestor.h"
#include "DataFilter.h"
#include "AIQueryDispatcher.h"

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <fstream>

using json = nlohmann::json;

static std::string makeError(const std::string &message)
{
    return json({{"status", "error"}, {"msg", message}}).dump();
}

/**
 * @brief 获取当前UTC时间
 *
 * 设计说明：
 * - 使用UTC时间避免时区和夏令时影响
 * - 使用 ISO8601+'Z'(Zulu)作为标准表示
 * - 该时间为系统"权威时间"(server_timestamp)
 *
 *
 * 示例：
 *   2026-4-16T21:35:10Z
 */
static std::string nowISo8601()
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
 * @brief PImpl结构体
 *
 * 设计目的：
 * - 将 httplib::Server从头文件中独立出来
 * - 降低编译依赖
 * - 提高编译独立度(避免头文件膨胀)
 * - 提升编译速度与模块隔离性
 *
 *
 * 本质：
 * - DataIngestor 只提供接口
 * - PImpl 提供实现
 */
struct DataIngestor::Impl
{
    httplib::Server server;
};

/**
 * @brief 构造函数(依赖注入)
 *
 * 设计说明：
 * - 使用引用 (&) 表示强依赖 (这些模块必须存在)
 * - 生命周期由外部控制 (DataIngestor不负责创建/销毁)
 * - 避免Copy,提高性能
 *
 */
DataIngestor::DataIngestor(
    StorageEngine &storage,
    DataFilter &filter,
    AIQueryDispatcher &ai,
    double temp_min, double temp_max,
    double hum_min,  double hum_max,
    std::vector<std::string> allowlist,
    std::string command_api_key)
    : storage_(storage), filter_(filter), ai_(ai)
    , temp_min_(temp_min), temp_max_(temp_max)
    , hum_min_(hum_min),   hum_max_(hum_max)
    , allowlist_(std::move(allowlist))
    , command_api_key_(std::move(command_api_key))
    , impl_(std::make_unique<Impl>())
{
}

/**
 * @brief 析构函数
 *
 * 说明：
 * - unique_ptr 自动释放 Impl
 * - 无需手动delete
 */
DataIngestor::~DataIngestor() = default;

/**
 * @brief 启动HTTP服务器(阻塞调用)
 *
 * 功能：
 * - 注册路由(/health,/api/ingest)
 * - 监听端口并进入事件循环
 *
 * 设计说明：
 * - 使用 lambda 捕获 this,调用类内部逻辑
 * - HTTP层与业务逻辑(handleIngest)解耦
 */
bool DataIngestor::start(const std::string &host,
                         int port,
                         int max_connections,
                         int request_timeout_ms)
{
    auto &svr = impl_->server;
    const int timeout_sec = std::max(1, request_timeout_ms / 1000);

    svr.set_read_timeout(timeout_sec);
    svr.set_write_timeout(timeout_sec);
    if (max_connections > 0)
    {
        svr.new_task_queue = [max_connections] {
            return new httplib::ThreadPool(static_cast<size_t>(max_connections));
        };
    }

    // 静态资源目录随工作目录变：从仓库根目录跑是 "frontend/dist"，
    // 从 build-cmake/backend/ 跑是 "../../frontend/dist"。写死一个的结果是
    // 按 README 启动时网页 404，而后端日志一切正常 —— 所以这里逐个试。
    static const char *kFrontendCandidates[] = {
        "frontend/dist", "../frontend/dist", "../../frontend/dist"};

    for (const char *dir : kFrontendCandidates)
    {
        if (svr.set_mount_point("/", dir))
        {
            frontend_dir_ = dir;
            break;
        }
    }

    if (frontend_dir_.empty())
        std::cerr << "[WARN] frontend/dist not found (tried repo root, ../, ../../)"
                     " — API-only mode\n";
    else
        std::cout << "[OK] Dashboard served from " << frontend_dir_ << "\n";

    // SPA 回退：非 API 的 404 一律返回 index.html，交给前端路由
    svr.set_error_handler([this](const httplib::Request &req, httplib::Response &res) {
        if (res.status == 404 && !frontend_dir_.empty() &&
            req.path.substr(0, 4) != "/api" &&
            req.path != "/health")
        {
            std::ifstream ifs(frontend_dir_ + "/index.html");
            if (ifs.good())
            {
                std::string html((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());
                res.set_content(html, "text/html");
                res.status = 200;
            }
        }
    });

    svr.Get("/health", [](const httplib::Request &, httplib::Response &res)
            { res.set_content(R"({"status":"ok"})", "application/json"); });

    svr.Post("/api/prompt",
             [this](const httplib::Request &req, httplib::Response &res)
             {
                 try
                 {
                     auto j = json::parse(req.body);
                     std::string device_id = j.value("device_id", "esp32s3-001");
                     std::string prompt = j.at("prompt").get<std::string>();

                     if (!isDeviceAllowed(device_id))
                     {
                         res.status = 401;
                         res.set_content(makeError("device not authorized"), "application/json");
                         return;
                     }

                     std::string answer = ai_.answerPrompt(device_id, prompt);
                     res.set_content(json({{"status", "ok"}, {"answer", answer}}).dump(),
                                    "application/json");
                 }
                 catch (const std::exception &e)
                 {
                     res.status = 500;
                     res.set_content(makeError(e.what()), "application/json");
                 }
             });

    svr.Get("/api/readings",
            [this](const httplib::Request &req, httplib::Response &res)
            {
                std::string response;
                int status = 200;
                handleReadings(req.get_param_value("device_id"),
                               req.get_param_value("limit"),
                               response, status);
                res.status = status;
                res.set_content(response, "application/json");
            });

    svr.Post("/api/ingest",
             [this](const httplib::Request &req, httplib::Response &res)
             {
                 std::string response;
                 int status = 200;
                 // 将HTTP body交给业务流水线处理
                 handleIngest(req.body, response, status);
                 res.status = status;
                 res.set_content(response, "application/json");
             });

    svr.Post("/api/commands",
             [this](const httplib::Request &req, httplib::Response &res)
             {
                 std::string response;
                 int status = 202;
                 handleCreateCommand(req.body, req.get_header_value("X-Api-Key"), response, status);
                 res.status = status;
                 res.set_content(response, "application/json");
             });

    svr.Get("/api/commands/next",
            [this](const httplib::Request &req, httplib::Response &res)
            {
                std::string response;
                int status = 200;
                handleNextCommand(req.get_param_value("device_id"), response, status);
                res.status = status;
                res.set_content(response, "application/json");
            });

    svr.Post("/api/commands/ack",
             [this](const httplib::Request &req, httplib::Response &res)
             {
                 std::string response;
                 int status = 200;
                 handleCommandAck(req.body, response, status);
                 res.status = status;
                 res.set_content(response, "application/json");
             });
    std::cout << "[DataIngestor] Listening on " << host << ":" << port
              << " (max_connections=" << max_connections
              << ", timeout_ms=" << request_timeout_ms << ")" << std::endl;

    // 阻塞运行(进入事件循环)
    const bool ok = svr.listen(host, port);
    if (!ok)
    {
        std::cerr << "[DataIngestor] Failed to listen on " << host << ":" << port << std::endl;
    }
    return ok;
}

/**
 * @brief 停止HTTP服务器
 *
 * - graceful shutdown
 *
 */
void DataIngestor::stop()
{
    impl_->server.stop();
}

/**
 * @brief 核心数据处理流水线(系统入口)
 *
 * 完整流程:
 *  JSON -> parse -> validate -> filter -> storage -> AI -> response
 *
 * 设计说明：
 * - 使用 vector<SensorReading> 统一数据类型
 * - 各模块职责单一(解耦)
 * - 失败即返回(fail-fast)
 */
void DataIngestor::handleIngest(const std::string &raw_json,
                                std::string &response_json,
                                int &status_code)
{
    std::vector<SensorReading> readings;
    std::optional<EdgeAssessment> edge;
    std::string err;

    // 1.JSON 解析
    if (!parseJson(raw_json, readings, edge, err))
    {
        status_code = 400;
        response_json = makeError(err);
        return;
    }

    // 2.数据校验
    if (!validate(readings, err))
    {
        status_code = err == "device not authorized" ? 401 : 400;
        response_json = makeError(err);
        return;
    }

    bool has_anomaly = false;

    // 3.异常检测 + 存储 (逐条处理)
    for (auto &r : readings)
    {

        // IQR异常检测（滑动窗口）
        bool is_anomaly = filter_.check(r);
        if (is_anomaly)
        {
            has_anomaly = true;
            storage_.insertAnomaly(r);
        }

        // 写入数据库
        if (!storage_.insertReading(r))
        {
            status_code = 500;
            response_json = makeError("failed to store reading");
            return;
        }
    }

    // 4.设备端推理结论落库（仅状态变化时真正写入）
    if (edge && !storage_.insertEdgeAssessment(*edge))
        std::cerr << "[Ingest] failed to store edge assessment for "
                  << edge->device_id << std::endl;

    // 5.AI 叙述（稳态下 onNewData 内部直接返回，不产生 LLM 调用）
    ai_.onNewData(readings, has_anomaly);

    // 6.返回响应（含 AI 分析，供设备端 OLED 显示）
    std::string analysis = ai_.getLastAnalysis(readings[0].device_id);
    json resp = {
        {"status", "ok"},
        {"anomaly", has_anomaly}
    };
    if (!analysis.empty())
        resp["analysis"] = analysis;
    response_json = resp.dump();

    // 日志输出(调试)
    std::cout << "[Ingest] device=" << readings[0].device_id
              << " count=" << readings.size()
              << (edge ? " edge=" + edge->state : "")
              << (has_anomaly ? " [ANOMALY]" : "")
              << std::endl;
}

namespace
{
// 固件按 REPORT_INTERVAL_MS = 10s 上报，连续错过三次才算掉线。
// 阈值贴着上报周期设会让偶发的一次重传就把设备判成离线，仪表盘会闪。
constexpr int kOfflineAfterSeconds = 30;

// ISO8601 UTC ("...Z") → epoch 秒。格式由本服务自己写入，可信。
bool parseIso8601Utc(const std::string &ts, int64_t &out)
{
    if (ts.size() < 19)
        return false;
    try
    {
        std::tm t = {};
        t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
        t.tm_mon = std::stoi(ts.substr(5, 2)) - 1;
        t.tm_mday = std::stoi(ts.substr(8, 2));
        t.tm_hour = std::stoi(ts.substr(11, 2));
        t.tm_min = std::stoi(ts.substr(14, 2));
        t.tm_sec = std::stoi(ts.substr(17, 2));
        out = static_cast<int64_t>(timegm(&t));
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}
}

void DataIngestor::handleReadings(const std::string &device_id,
                                  const std::string &limit_param,
                                  std::string &response_json,
                                  int &status_code)
{
    if (device_id.empty())
    {
        status_code = 400;
        response_json = makeError("device_id is required");
        return;
    }
    if (!isDeviceAllowed(device_id))
    {
        status_code = 401;
        response_json = makeError("device not authorized");
        return;
    }

    // limit 是每种量纲的点数上限。取两倍是因为 getRecentReadings 按行返回，
    // 温湿度交替各占一行。
    int limit = 120;
    if (!limit_param.empty())
    {
        try
        {
            limit = std::stoi(limit_param);
        }
        catch (const std::exception &)
        {
            status_code = 400;
            response_json = makeError("limit must be an integer");
            return;
        }
        if (limit < 1 || limit > 1000)
        {
            status_code = 400;
            response_json = makeError("limit must be between 1 and 1000");
            return;
        }
    }

    // DESC（最新在前）→ 反转成时间正序，前端画图不用再排
    auto rows = storage_.getRecentReadings(device_id, limit * 2);
    std::reverse(rows.begin(), rows.end());

    json series = json::object();
    std::string last_seen;
    for (const auto &r : rows)
    {
        if (!series.contains(r.sensor_type))
            series[r.sensor_type] = {{"unit", r.unit}, {"points", json::array()}};

        series[r.sensor_type]["points"].push_back({
            {"timestamp", r.server_timestamp},
            {"value", r.value}
        });

        if (r.server_timestamp > last_seen)
            last_seen = r.server_timestamp;
    }

    const std::string now = nowISo8601();

    // 在线判定基于最后一条读数的时间，而不是 HTTP 是否可达 ——
    // 后端活着不代表设备还在上报，这两件事必须分开显示。
    bool online = false;
    int64_t last_epoch = 0, now_epoch = 0;
    if (!last_seen.empty() && parseIso8601Utc(last_seen, last_epoch) &&
        parseIso8601Utc(now, now_epoch))
    {
        online = (now_epoch - last_epoch) <= kOfflineAfterSeconds;
    }

    json resp = {
        {"status", "ok"},
        {"device_id", device_id},
        {"server_time", now},
        {"online", online},
        {"series", series}
    };
    resp["last_seen"] = last_seen.empty() ? json(nullptr) : json(last_seen);

    // 设备端 EdgeReasoner 的结论。since = 该状态开始的时刻（只在变化时落库）。
    EdgeAssessment edge;
    if (storage_.getLatestEdgeAssessment(device_id, edge))
    {
        resp["edge"] = {
            {"state", edge.state},
            {"severity", edge.severity},
            {"confidence", edge.confidence},
            {"reason_code", edge.reason_code},
            {"reason", edge.reason},
            {"since", edge.timestamp}
        };
    }
    else
    {
        resp["edge"] = nullptr;
    }

    status_code = 200;
    response_json = resp.dump();
}

void DataIngestor::handleCreateCommand(const std::string &raw_json,
                                       const std::string &api_key,
                                       std::string &response_json,
                                       int &status_code)
{
    try
    {
        if (!isCommandApiAuthorized(api_key))
        {
            status_code = 401;
            response_json = makeError("command API key required");
            return;
        }

        auto j = json::parse(raw_json);
        const std::string device_id = j.at("device_id").get<std::string>();
        const std::string command = j.at("command").get<std::string>();
        int duration_ms = j.value("duration_ms", 0);

        if (!isDeviceAllowed(device_id))
        {
            status_code = 401;
            response_json = makeError("device not authorized");
            return;
        }
        if (!isAllowedCommand(command))
        {
            status_code = 400;
            response_json = makeError("command not allowed");
            return;
        }

        // 每种命令的合法区间由固件决定，这里必须跟着它，否则后端会接受一条
        // 设备一定会拒绝的命令：队列里显示 queued，实际永远执行不了。
        std::string range_error;
        if (!normalizeCommandDuration(command, duration_ms, range_error))
        {
            status_code = 400;
            response_json = makeError(range_error);
            return;
        }

        if (command.rfind("oled:", 0) == 0)
        {
            std::string text_error;
            if (!isDisplayableOledText(command.substr(5), text_error))
            {
                status_code = 400;
                response_json = makeError(text_error);
                return;
            }
        }

        int command_id = 0;
        if (!storage_.enqueueDeviceCommand(device_id, command, duration_ms, &command_id))
        {
            status_code = 500;
            response_json = makeError("failed to queue command");
            return;
        }

        status_code = 202;
        response_json = json({
            {"status", "queued"},
            {"command_id", command_id},
            {"device_id", device_id},
            {"command", command},
            {"duration_ms", duration_ms}
        }).dump();
    }
    catch (const std::exception &e)
    {
        status_code = 400;
        response_json = makeError(e.what());
    }
}

void DataIngestor::handleNextCommand(const std::string &device_id,
                                     std::string &response_json,
                                     int &status_code)
{
    if (device_id.empty())
    {
        status_code = 400;
        response_json = makeError("device_id is required");
        return;
    }
    if (!isDeviceAllowed(device_id))
    {
        status_code = 401;
        response_json = makeError("device not authorized");
        return;
    }

    DeviceCommand command;
    if (!storage_.getPendingDeviceCommand(device_id, command))
    {
        status_code = 200;
        response_json = json({{"status", "idle"}}).dump();
        return;
    }

    status_code = 200;
    response_json = json({
        {"status", "ok"},
        {"command_id", command.id},
        {"command", command.command},
        {"duration_ms", command.duration_ms}
    }).dump();
}

void DataIngestor::handleCommandAck(const std::string &raw_json,
                                    std::string &response_json,
                                    int &status_code)
{
    try
    {
        auto j = json::parse(raw_json);
        const std::string device_id = j.at("device_id").get<std::string>();
        const int command_id = j.at("command_id").get<int>();
        const std::string result = j.value("result", "done");

        if (!isDeviceAllowed(device_id))
        {
            status_code = 401;
            response_json = makeError("device not authorized");
            return;
        }
        if (command_id <= 0)
        {
            status_code = 400;
            response_json = makeError("command_id must be positive");
            return;
        }
        if (result != "done" && result != "failed")
        {
            status_code = 400;
            response_json = makeError("result must be done or failed");
            return;
        }
        if (!storage_.ackDeviceCommand(device_id, command_id, result))
        {
            status_code = 404;
            response_json = makeError("pending command not found");
            return;
        }

        status_code = 200;
        response_json = json({{"status", "acked"}, {"command_id", command_id}}).dump();
    }
    catch (const std::exception &e)
    {
        status_code = 400;
        response_json = makeError(e.what());
    }
}

/**
 * @brief JSON解析->统一数据模型(双时间)
 *
 * 输入：
 *    原始JSON字符串
 *
 * 输出：
 *    vector<SensorReading>
 *
 * 设计说明：
 * - 将不同传感器拆分为统一结构
 * - 只解析硬件真实存在的量（SHT30：temperature/humidity）；
 *   其余字段忽略，全部忽略时按 "no supported sensor fields" 拒绝
 * - 同时生成：
 *       device_timestamp(设备时间)
 *       server_timestamp(UTC权威时间)
 */
bool DataIngestor::parseJson(const std::string &raw,
                             std::vector<SensorReading> &out,
                             std::optional<EdgeAssessment> &out_edge,
                             std::string &error_msg)
{
    try
    {
        auto j = json::parse(raw);

        std::string device_id = j.at("device_id").get<std::string>();

        // 设备时间
        std::string device_ts;
        if (j.contains("timestamp"))
        {
            const auto &ts = j.at("timestamp");
            if (ts.is_string())
            {
                device_ts = ts.get<std::string>();
            }
            else if (ts.is_number_integer())
            {
                device_ts = std::to_string(ts.get<long long>());
            }
            else if (ts.is_number_float())
            {
                device_ts = std::to_string(ts.get<double>());
            }
            else if (ts.is_null())
            {
                device_ts.clear();
            }
            else
            {
                throw std::runtime_error("timestamp must be string or number");
            }
        }
        // 服务器时间(UTC)
        std::string server_ts = nowISo8601();

        // 设备端推理结果（EdgeReasoner 的判定，随读数一起上报）
        //
        // 缺失是合法的：模拟器和早期固件都不带这个字段，此时只是没有推理结论，
        // 不影响读数本身。字段在但结构不对才算错误 —— 静默吞掉会让固件那边
        // 以为上报成功了，而网页上永远看不到推理结果。
        if (j.contains("edge") && !j.at("edge").is_null())
        {
            const auto &e = j.at("edge");
            if (!e.is_object())
                throw std::runtime_error("edge must be an object");

            EdgeAssessment a;
            a.device_id = device_id;
            a.state = e.value("state", "");
            a.severity = e.value("severity", "");
            a.confidence = e.value("confidence", 0.0);
            a.reason_code = e.value("reason_code", "");
            a.reason = e.value("reason", "");
            a.timestamp = server_ts;

            if (a.state.empty())
                throw std::runtime_error("edge.state is required");

            out_edge = std::move(a);
        }

        // 温度
        if (j.contains("temperature"))
        {
            SensorReading t;

            t.device_id = device_id;
            t.sensor_type = "temperature";
            t.value = j.at("temperature").get<double>();
            t.unit = "C";

            t.device_timestamp = device_ts; // 参考时间
            t.server_timestamp = server_ts; // 权威时间
            t.timestamp = server_ts;

            out.push_back(t);
        }
        // humidity
        if (j.contains("humidity"))
        {
            SensorReading h;

            h.device_id = device_id;
            h.sensor_type = "humidity";
            h.value = j.at("humidity").get<double>();
            h.unit = "%";

            h.device_timestamp = device_ts;
            h.server_timestamp = server_ts;
            h.timestamp = server_ts;

            out.push_back(h);
        }
        if (out.empty())
        {
            error_msg = "no supported sensor fields";
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        error_msg = e.what();
        std::cerr << "[Parse Error] " << error_msg << std::endl;
        return false;
    }
}

/**
 * @brief 数据校验
 *
 * 功能：
 * - 检验字段完整性
 * - 校验数值范围
 *
 * 设计说明：
 * - 按 sensor_type 分支
 * - fail-fast：一旦错误立即返回
 */
bool DataIngestor::validate(const std::vector<SensorReading> &readings,
                            std::string &error_msg)
{
    if (readings.empty())
    {
        error_msg = "no readings provided";
        return false;
    }

    for (const auto &r : readings)
    {
        // 1. allowlist check（空列表=允许全部）
        if (r.device_id.empty())
        {
            error_msg = "device id is missing";
            return false;
        }
        if (!isDeviceAllowed(r.device_id))
        {
            error_msg = "device not authorized";
            return false;
        }

        // temp
        if (r.sensor_type == "temperature")
        {
            if (r.value < temp_min_ || r.value > temp_max_)
            {
                error_msg = "temperature is out of the range";
                return false;
            }
        }
        else if(r.sensor_type=="humidity"){
            if(r.value < hum_min_ || r.value > hum_max_){
                error_msg="humidity is out of the range";
                return false;
            }
        }
        else{
            error_msg="unknown sensor type:"+r.sensor_type;
            return false;
        }
    }
    return true;
}

bool DataIngestor::isDeviceAllowed(const std::string &device_id) const
{
    if (device_id.empty())
        return false;
    if (allowlist_.empty())
        return true;
    return std::find(allowlist_.begin(), allowlist_.end(), device_id) != allowlist_.end();
}

bool DataIngestor::isCommandApiAuthorized(const std::string &api_key) const
{
    return command_api_key_.empty() || api_key == command_api_key_;
}

bool DataIngestor::isAllowedCommand(const std::string &command)
{
    return command == "buzzer_on" || command == "buzzer_off" ||
           command.rfind("oled:", 0) == 0;
}

/**
 * @brief 按命令类型校验并归一化 duration_ms
 *
 * 区间来自固件，不是这里随便定的：
 *  - 蜂鸣器：0..30000，0 表示用默认 1000
 *  - OLED  ：oled.cpp 的 showCustomMessage() 要求 5000..120000。
 *            0 表示"用设备默认值"，这里显式写成 30000 —— 队列里存下来的
 *            就是设备真正会执行的时长，不用回头去猜固件的默认是多少。
 *
 * 改这些数值前先看 firmware/combined/src/oled.cpp 的
 * CUSTOM_MIN_DURATION_MS / CUSTOM_MAX_DURATION_MS。
 */
bool DataIngestor::normalizeCommandDuration(const std::string &command,
                                            int &duration_ms,
                                            std::string &error_msg)
{
    constexpr int kBuzzerMaxMs = 30000;
    constexpr int kBuzzerDefaultMs = 1000;
    constexpr int kOledMinMs = 5000;
    constexpr int kOledMaxMs = 120000;
    constexpr int kOledDefaultMs = 30000;

    if (duration_ms < 0)
    {
        error_msg = "duration_ms must not be negative";
        return false;
    }

    if (command.rfind("oled:", 0) == 0)
    {
        if (duration_ms == 0)
        {
            duration_ms = kOledDefaultMs;
            return true;
        }
        if (duration_ms < kOledMinMs || duration_ms > kOledMaxMs)
        {
            error_msg = "duration_ms for oled: must be 0 or between " +
                        std::to_string(kOledMinMs) + " and " +
                        std::to_string(kOledMaxMs) +
                        " (the device rejects anything else)";
            return false;
        }
        return true;
    }

    if (duration_ms > kBuzzerMaxMs)
    {
        error_msg = "duration_ms must be between 0 and " +
                    std::to_string(kBuzzerMaxMs);
        return false;
    }

    if (command == "buzzer_on" && duration_ms == 0)
        duration_ms = kBuzzerDefaultMs;
    if (command == "buzzer_off")
        duration_ms = 0;
    return true;
}

/**
 * @brief 校验 OLED 文本是否可显示
 *
 * 镜像 firmware/combined/src/oled.cpp 的 isDisplayableText()：
 * 屏幕没有中文字库，固件会逐字节拒绝范围外的内容。后端提前拦下来，
 * 是为了给出说得清的错误，而不是让命令排进队列再被设备静默丢掉。
 */
bool DataIngestor::isDisplayableOledText(const std::string &text,
                                         std::string &error_msg)
{
    constexpr size_t kMaxBytes = 240;

    if (text.empty())
    {
        error_msg = "oled: text must not be empty";
        return false;
    }
    if (text.size() > kMaxBytes)
    {
        error_msg = "oled: text must be at most " +
                    std::to_string(kMaxBytes) + " bytes";
        return false;
    }

    bool has_visible = false;
    for (const char c : text)
    {
        const unsigned char byte = static_cast<unsigned char>(c);
        const int normalized = std::toupper(byte);
        if (normalized < ' ' || normalized > 'Z')
        {
            error_msg = "oled: text must be printable ASCII — "
                        "the display has no font for anything else";
            return false;
        }
        if (byte != ' ')
            has_visible = true;
    }

    if (!has_visible)
    {
        error_msg = "oled: text must contain a visible character";
        return false;
    }
    return true;
}
