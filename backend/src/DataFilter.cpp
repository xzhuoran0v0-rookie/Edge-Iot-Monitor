#include "DataFilter.h"

#include <algorithm>
#include <iostream>
#include <ctime>

/**
 * @brief 构造函数
 *
 * @param window_seconds 滑动窗口大小
 * @param iqr_multiplier IQR倍数
 *
 */
DataFilter::DataFilter(int window_seconds, double iqr_multiplier, int min_samples)
    : window_seconds_(window_seconds), iqr_multiplier_(iqr_multiplier), min_samples_(min_samples)
{
}

// ————————————————————————————————————————
// 主入口：检测+更新窗口
// ————————————————————————————————————————
bool DataFilter::check(const SensorReading &reading)
{
    // 整个 检测+更新窗口 流程持锁，防止多个请求线程并发读写 windows_
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- 1.解析 server_timestamp 为 epoch 秒 ----
    const std::string &ts = !reading.server_timestamp.empty() ? reading.server_timestamp : reading.timestamp;

    int64_t now_sec = 0;
    if (ts.size() >= 19)
    // "2026-05-26T16:15:38Z" → 至少 19 字符
    {
        // 手动解析 ISO8601，避免依赖 <chrono> 的 from_chars,避免macOS不支持的情况
        // 格式：YYYY-MM-DDTHH:MM:SSZ
        try
        {
            struct tm t = {};
            t.tm_year = std::stoi(ts.substr(0, 4)) - 1900;
            t.tm_mon = std::stoi(ts.substr(5, 2)) - 1;
            t.tm_mday = std::stoi(ts.substr(8, 2));
            t.tm_hour = std::stoi(ts.substr(11, 2));
            t.tm_min = std::stoi(ts.substr(14, 2));
            t.tm_sec = std::stoi(ts.substr(17, 2));
            now_sec = timegm(&t); // UTC → epoch 秒
        }
        catch (const std::exception &)
        {
            // 时间戳格式异常时回退到系统时间，不让请求线程带着异常逃逸
            now_sec = static_cast<int64_t>(std::time(nullptr));
        }
    }

    // ---- 2. 找到窗口，淘汰过期数据 ----
    const auto key = makeKey(reading.device_id, reading.sensor_type);
    auto &window = windows_[key];

    const int64_t cutoff = now_sec - window_seconds_;
    while (!window.empty() && window.front().unix_sec < cutoff)
    {
        window.pop_front();
    }

    // ---- 3.窗口足够，运行IQR ----
    bool is_anomaly = false;
    if (static_cast<int>(window.size()) >= min_samples_)
    {
        std::vector<double> sorted;
        sorted.reserve(window.size());
        for (const auto &s : window)
            sorted.push_back(s.value);

        std::sort(sorted.begin(), sorted.end());

        double lower, upper;
        computeBounds(sorted, lower, upper);

        if (reading.value < lower || reading.value > upper)
        {
            is_anomaly = true;
            std::cout << "[DataFilter] ANOMALY "
                      << reading.device_id << "/" << reading.sensor_type
                      << " value=" << reading.value
                      << " bounds=[" << lower << "," << upper << "]"
                      << std::endl;
        }
    }

    // ---- 4.存入新数据 ----
    window.push_back({reading.value, now_sec});
    return is_anomaly;
}

// ─────────────────────────────────────────────
// 生成 map key（device_id|sensor_type）
// ─────────────────────────────────────────────
std::string DataFilter::makeKey(const std::string &device_id,
                                const std::string &sensor_type)
{
    return device_id + "|" + sensor_type;
}

// ─────────────────────────────────────────────
// 计算 IQR 上下界
// ─────────────────────────────────────────────
void DataFilter::computeBounds(const std::vector<double> &sorted_vals,
                               double &lower,
                               double &upper) const
{
    const int n = static_cast<int>(sorted_vals.size());

    /**
     * 边界保护：
     * - 数据过少时(<4)IQR不稳定
     * - 直接使用max/min作为边界
     */
    if (n < 4)
    {
        lower = sorted_vals.front();
        upper = sorted_vals.back();
        return;
    }

    /**
     * 辅助函数计算区间中位数
     * 区间：[lo,hi]
     */
    auto median = [](const std::vector<double> &v, int lo, int hi) -> double
    {
        int len = hi - lo + 1;
        int mid = lo + len / 2;

        if (len % 2 == 1)
        {
            return v[mid];
        }
        else
        {
            return (v[mid - 1] + v[mid]) / 2.0;
        }
    };

    // Q1(下半部分中位数)
    double q1 = median(sorted_vals, 0, n / 2 - 1);

    // Q3(上半部分中位数)
    double q3 = median(sorted_vals, n / 2 + (n % 2), n - 1);

    double iqr = q3 - q1;

    // IQR区间
    lower = q1 - iqr_multiplier_ * iqr;
    upper = q3 + iqr_multiplier_ * iqr;
}
