#include "DataIngestor.h"
#include "DataFilter.h"
#include "AIQueryDispatcher.h"
#include "CloudSync.h"

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
    CloudSync &cloud,
    double temp_min, double temp_max,
    double hum_min,  double hum_max,
    std::vector<std::string> allowlist,
    std::string command_api_key)
    : storage_(storage), filter_(filter), ai_(ai), cloud_(cloud)
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

    auto ret = svr.set_mount_point("/", "../frontend/dist");
    if (!ret)
        std::cerr << "[WARN] Static mount ../frontend/dist not found — API-only mode\n";

    svr.set_error_handler([&svr](const httplib::Request &req, httplib::Response &res) {
        if (res.status == 404 && req.path.substr(0, 4) != "/api" &&
            req.path != "/health")
        {
            std::ifstream ifs("../frontend/dist/index.html");
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
    std::string err;

    // 1.JSON 解析
    if (!parseJson(raw_json, readings, err))
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

    // 4.AI 叙述（稳态下 onNewData 内部直接返回，不产生 LLM 调用）
    ai_.onNewData(readings, has_anomaly);

    // 5.云同步
    cloud_.onNewData(readings);

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
              << (has_anomaly ? " [ANOMALY]" : "")
              << std::endl;
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
        if (duration_ms < 0 || duration_ms > 30000)
        {
            status_code = 400;
            response_json = makeError("duration_ms must be between 0 and 30000");
            return;
        }
        if (command == "buzzer_on" && duration_ms == 0)
            duration_ms = 1000;
        if (command == "buzzer_off")
            duration_ms = 0;

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
           command.substr(0, 5) == "oled:";
}
