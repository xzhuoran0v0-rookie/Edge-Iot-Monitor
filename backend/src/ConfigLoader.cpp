#include "ConfigLoader.h"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <iostream>
#include <fstream>

/**
 * @brief 环境变量覆盖（优先级高于 yaml）
 *
 * 目前仅 DEEPSEEK_API_KEY — 便于不把密钥写进配置文件。
 */
static void applyEnvOverrides(AppConfig &cfg)
{
    if (const char *key = std::getenv("DEEPSEEK_API_KEY"); key && *key)
        cfg.deepseek_api_key = key;
}

AppConfig loadConfig(const std::string &path)
{
    AppConfig cfg;

    std::ifstream f(path);
    if (!f.is_open())
    {
        std::cerr << "[Config] Cannot open " << path
                  << " — using defaults.\n";
        applyEnvOverrides(cfg);
        return cfg;
    }

    try
    {
        YAML::Node root = YAML::LoadFile(path);

        // ---- server ----
        if (root["server"])
        {
            auto s = root["server"];
            if (s["host"])             cfg.server_host = s["host"].as<std::string>();
            if (s["port"])             cfg.server_port = s["port"].as<int>();
        }

        // ---- sqlite ----
        if (root["sqlite"])
        {
            auto db = root["sqlite"];
            if (db["db_path"])         cfg.db_path = db["db_path"].as<std::string>();
        }

        // ---- ollama ----
        if (root["ollama"])
        {
            auto o = root["ollama"];
            if (o["host"])             cfg.ollama_host = o["host"].as<std::string>();
            if (o["port"])             cfg.ollama_port = o["port"].as<int>();
            if (o["model"])            cfg.ollama_model = o["model"].as<std::string>();
            if (o["timeout_s"])        cfg.ollama_timeout_s = o["timeout_s"].as<int>();
        }

        // ---- deepseek ----
        if (root["deepseek"])
        {
            auto d = root["deepseek"];
            if (d["enabled"])          cfg.deepseek_enabled = d["enabled"].as<bool>();
            if (d["base_url"])         cfg.deepseek_base_url = d["base_url"].as<std::string>();
            if (d["model"])            cfg.deepseek_model = d["model"].as<std::string>();
            if (d["api_key"])          cfg.deepseek_api_key = d["api_key"].as<std::string>();
            if (d["timeout_s"])        cfg.deepseek_timeout_s = d["timeout_s"].as<int>();
        }

        // ---- filter ----
        if (root["filter"])
        {
            auto flt = root["filter"];
            if (flt["window_seconds"])     cfg.filter_window_seconds = flt["window_seconds"].as<int>();
            if (flt["iqr_multiplier"])     cfg.filter_iqr_multiplier = flt["iqr_multiplier"].as<double>();
            if (flt["min_window_samples"]) cfg.filter_min_samples = flt["min_window_samples"].as<int>();
        }

        // ---- ai ----
        if (root["ai"])
        {
            auto a = root["ai"];
            if (a["trigger_every_n_records"]) cfg.ai_trigger_count = a["trigger_every_n_records"].as<int>();
            if (a["max_window_records"])      cfg.ai_window_size = a["max_window_records"].as<int>();
            if (a["enabled"])                 cfg.ai_enabled = a["enabled"].as<bool>();
        }

        // ---- validation ----
        if (root["validation"])
        {
            auto v = root["validation"];
            if (v["temperature"])
            {
                if (v["temperature"]["min"]) cfg.temp_min = v["temperature"]["min"].as<double>();
                if (v["temperature"]["max"]) cfg.temp_max = v["temperature"]["max"].as<double>();
            }
            if (v["humidity"])
            {
                if (v["humidity"]["min"]) cfg.hum_min = v["humidity"]["min"].as<double>();
                if (v["humidity"]["max"]) cfg.hum_max = v["humidity"]["max"].as<double>();
            }
        }

        // ---- device allowlist ----
        if (root["devices"] && root["devices"]["allowlist"])
        {
            auto list = root["devices"]["allowlist"];
            for (const auto &item : list)
                cfg.device_allowlist.push_back(item.as<std::string>());
        }

        // ---- cloud ----
        if (root["cloud"])
        {
            auto c = root["cloud"];
            if (c["enabled"])        cfg.cloud_enabled = c["enabled"].as<bool>();
            if (c["provider"])       cfg.cloud_provider = c["provider"].as<std::string>();
            if (c["endpoint"])       cfg.cloud_endpoint = c["endpoint"].as<std::string>();
            if (c["project_id"])     cfg.cloud_project_id = c["project_id"].as<std::string>();
            if (c["device_id"])      cfg.cloud_device_id = c["device_id"].as<std::string>();
            if (c["credential"])     cfg.cloud_credential = c["credential"].as<std::string>();
        }

        std::cout << "[Config] Loaded " << path << "\n";
    }
    catch (const YAML::Exception &e)
    {
        std::cerr << "[Config] Parse error in " << path << ": "
                  << e.what() << " — using defaults.\n";
    }

    applyEnvOverrides(cfg);
    return cfg;
}
