#include "DataFilter.h"

#include <algorithm>
#include <iostream>

/**
 * @brief 构造函数
 *
 * @param window_size 滑动窗口大小
 * @param iqr_multiplier IQR倍数
 *
 */
DataFilter::DataFilter(int window_size, double iqr_multiplier)
    : window_size_(window_size), iqr_multiplier_(iqr_multiplier)
{
}

// ————————————————————————————————————————
// 主入口：检测+更新窗口
// ————————————————————————————————————————
bool DataFilter::check(const SensorReading &reading)
{
    const auto key = makeKey(reading.device_id, reading.sensor_type);

    // 若不存在则自动创建新窗口
    auto &window = windows_[key];

    bool is_anomaly = false;

    /**
     * 设计：
     * - 窗口未满->不启动检测
     * - 窗口已满->使用IQR检测
     */
    if (static_cast<int>(window.size()) >= window_size_)
    {
        // Copy and sort
        std::vector<double> sorted(window.begin(), window.end());
        std::sort(sorted.begin(), sorted.end());

        double lower, upper;

        computeBounds(sorted, lower, upper);

        if (reading.value < lower || reading.value > upper)
        {
            is_anomaly = true;

            // 调试输出
            std::cout << "[DataFilter] ANOMALY"
                      << reading.device_id << "/" << reading.sensor_type
                      << "value=" << reading.value
                      << "bounds=[" << lower << "," << upper << "]"
                      << std::endl;
        }
    }
    // ── 更新滑动窗口（始终执行） ──
    window.push_back(reading.value);

    // 超出窗口大小则移除最旧数据
    if (static_cast<int>(window.size()) > window_size_)
    {
        window.pop_front();
    }
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
