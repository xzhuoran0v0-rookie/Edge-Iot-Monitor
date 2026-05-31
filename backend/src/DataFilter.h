#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "StorageEngine.h"

/**
 * @brief 基于滑动窗口+IQR的在线异常检测器
 *
 * 设计说明：
 * - 每个(device_id+sensor_type)维护独立窗口
 * - 窗口按时间顺序存储最近N条数据(default 60)
 * - 当窗口未满：只积累数据，不做异常判断
 * - 当窗口满：使用IQR检测异常
 *
 * 判定规则：
 *  IQR=Q3-Q1
 *  lower=P1-k*IQR
 *  upper=Q3+k*IQR
 *
 *
 */
class DataFilter
{
public:
    /**
     * @brief 构造函数
     *
     * @param window_seconds    滑动窗口大小
     * @param iqr_multiplier IQR倍数
     */
    explicit DataFilter(int window_seconds = 60, double iqr_multiplier = 1.5,int min_samples=10);

    /**
     * @brief 检测一条数据是否正常,并更新窗口
     *
     * 行为：
     * 1.若未满->仅加入数据，返回false
     * 2.已满->计算IQR区间
     * 3.判断是否越界
     * 4.更新窗口
     *
     * @param reading 传入传感器数据
     * @return true=异常，false=正常
     *
     *
     */
    bool check(const SensorReading &reading);

    /**
     * 清空某设备的类型滑动窗口
     *
     * 使用场景：
     * - 设备重启
     * - 数据突变
     *
     * @param device_id 设备id
     * @param sensor_type 传感器类型
     */
    void reset(const std::string &device_id,
               const std::string &sensor_type);

private:
    /**
     * @brief 使得每条数据同时记录数值以及时间
     * 
     * @param value 数值
     * @param unix_sec epoch秒，计算距离上次记录过去的时间
     */
    struct WindowSample{
        double value;
        int64_t unix_sec;
    };

    int window_seconds_;        ///< 窗口大小
    double iqr_multiplier_; ///< IQR倍数
    int min_samples_;       ///< 最小样本数

    /**
     * @brife 滑动窗口存储
     *
     * key："device_id|sensor_type"
     * value：按时间存储历史值
     *
     *
     */
    std::unordered_map<std::string, std::deque<WindowSample>> windows_;

    /**
     * @brief 构造 map key
     *
     * @param device_id
     * @param sensor_type
     * @return 拼接后的key
     */
    static std::string makeKey(const std::string &device_id,
                               const std::string &sensor_type);

    /**
     * @brief 计算IQR上下限
     * @param sorted_vals (升序)
     * @param lower 下限
     * @param upper 上限
     *
     */
    void computeBounds(const std::vector<double> &sorted_vals,
                       double &lower,
                       double &upper) const;
};
