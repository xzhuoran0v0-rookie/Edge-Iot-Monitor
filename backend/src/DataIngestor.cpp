#include "DataIngestor.h"
#include "DataFilter.h"
#include "AIQueryDispatcher.h"

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

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
    AIQueryDispatcher &ai)
    : storage_(storage), filter_(filter), ai_(ai), impl_(std::make_unique<Impl>())
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
void DataIngestor::start(int port)
{
    auto &svr = impl_->server;

    // 健康检查接口(用于测试服务是否存活)
    svr.Get("/health", [](const httplib::Request &, httplib::Response &res)
            { res.set_content(R"({"status":"ok"})", "application/json"); });

    svr.Post("/api/ingest",
             [this](const httplib::Request &req, httplib::Response &res)
             {
                 std::string response;
                 // 将HTTP body交给业务流水线处理
                 handleIngest(req.body, response);
                 res.set_content(response, "application/json");
             });
    std::cout << "[DataIngestor] Listening on 0.0.0.0:" << port << std::endl;

    // 阻塞运行(进入事件循环)
    svr.listen("0.0.0.0", port);
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
                                std::string &response_json)
{
    std::vector<SensorReading> readings;
    std::string err;

    // 1.JSON 解析
    if (!parseJson(raw_json, readings, err))
    {
        response_json = R"({"status":"error","msg":")" + err + R"("})";
        return;
    }

    // 2.数据校验
    if (!validate(readings, err))
    {
        response_json = R"({"status":"error","msg":")" + err + R"("})";
        return;
    }

    bool has_anomaly = false;

    // 3.异常检测 + 存储 (逐条处理)
    for (auto &r : readings)
    {

        // IQR异常检测（滑动窗口）
        bool is_anomaly = filter_.check(r);
        if (is_anomaly)
            has_anomaly = true;

        // 写入数据库
        storage_.insertReading(r);
    }

    // 4.AI分析
    ai_.onNewData(readings);

    // 5.返回响应
    response_json = R"({"status":"ok","anomaly":)" +
                    std::string(has_anomaly ? "true" : "false") +
                    "}";

    // 日志输出(调试)
    std::cout << "[Ingest] device=" << readings[0].device_id
              << "count=" << readings.size()
              << (has_anomaly ? "[ANOMALY]" : "")
              << std::endl;
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
 * - 支持未来扩展(pressure等)
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
        std::string device_ts = j.contains("timestamp")
                                    ? j.at("timestamp").get<std::string>()
                                    : "";
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
    for (const auto &r : readings)
    {
        if (r.device_id.empty())
        {
            error_msg = "device id is missing";
            return false;
        }

        // temp
        if (r.sensor_type == "temperature")
        {
            if (r.value < -40.0 || r.value > 125.0)
            {
                error_msg = "temperature is out of the range";
                return false;
            }
        }
        else if(r.sensor_type=="humidity"){
            if(r.value<0.01||r.value>100.0){
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
