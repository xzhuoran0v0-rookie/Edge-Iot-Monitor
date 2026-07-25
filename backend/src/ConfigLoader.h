#pragma once

#include <string>
#include <vector>

/**
 * @brief 应用全局配置（从 config.yaml 加载）
 *
 * 每个字段都有默认值 — config.yaml 中缺失的项不会导致崩溃。
 */
struct AppConfig
{
    // ---- server ----
    std::string server_host = "0.0.0.0";
    int server_port = 8080;
    int server_max_connections = 32;
    int server_request_timeout_ms = 5000;

    // ---- sqlite ----
    std::string db_path = "sensor.db";

    // ---- ollama（本地备用推理后端） ----
    std::string ollama_host = "http://127.0.0.1";
    int ollama_port = 11434;
    std::string ollama_model = "qwen2.5:3b";
    int ollama_timeout_s = 120;
    int ollama_max_tokens = 512;
    double ollama_temperature = 0.3;

    // ---- deepseek（云端首选推理后端） ----
    // api_key 可由环境变量 DEEPSEEK_API_KEY 覆盖（优先级高于 yaml）
    bool deepseek_enabled = false;
    std::string deepseek_base_url = "https://api.deepseek.com";
    std::string deepseek_model = "deepseek-chat";
    std::string deepseek_api_key;
    int deepseek_timeout_s = 30;

    // ---- filter ----
    int filter_window_seconds = 60;
    double filter_iqr_multiplier = 1.5;
    int filter_min_samples = 10;

    // ---- ai dispatcher ----
    int ai_window_size = 100;
    bool ai_enabled = true;

    // ---- ai narration trigger（状态变化触发，取代旧的 ai.trigger_count） ----
    // 旧的按条数触发会在环境稳定时反复分析同一批数据，产出没有信息量。
    int ai_min_interval_s = 60;      ///< 同一设备两次叙述的最小间隔（秒）
    double ai_temp_delta_c = 2.0;    ///< 温度漂移触发阈值
    double ai_humidity_delta = 5.0;  ///< 湿度漂移触发阈值（%RH）
    double ai_warn_temp_c = 35.0;    ///< 温度告警带边界（跨越即触发）
    double ai_warn_humidity = 80.0;  ///< 湿度告警带边界（%RH）

    // ---- validation ----
    double temp_min = -40.0;
    double temp_max = 85.0;
    double hum_min = 0.0;
    double hum_max = 100.0;

    // ---- device allowlist ----
    std::vector<std::string> device_allowlist;

    // ---- local command API security ----
    std::string command_api_key;

    // ---- cloud sync ----
    bool cloud_enabled = false;
    std::string cloud_provider = "huawei";
    std::string cloud_endpoint;
    std::string cloud_project_id;
    std::string cloud_device_id;
    std::string cloud_credential;

    // ---- convenience ----
    /** 构造完整的 Ollama URL，如 "http://127.0.0.1:11434" */
    std::string ollamaUrl() const
    {
        return ollama_host + ":" + std::to_string(ollama_port);
    }
};

/**
 * @brief 从 YAML 文件加载配置
 *
 * @param path 配置文件路径（如 "config/config.yaml"）
 * @return 解析后的 AppConfig；文件不存在或解析失败时返回默认值
 */
AppConfig loadConfig(const std::string &path);
