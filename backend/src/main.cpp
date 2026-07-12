#include "StorageEngine.h"
#include "DataFilter.h"
#include "AIQueryDispatcher.h"
#include "DataIngestor.h"
#include "CloudSync.h"
#include "ConfigLoader.h"

#include <iostream>
#include <csignal>
#include <atomic>

// 全局运行标志(用于优雅退出)
std::atomic<bool> g_running(true);

// 信号处理
void signalHandler(int signum)
{
    std::cout << "\n[INFO] Interrupt signal (" << signum << ") received.\n";
    g_running = false;
}

int main()
{
    // 注册信号
    std::signal(SIGINT, signalHandler);

    // 0. 加载配置（config.yaml → 结构体，缺失项用默认值）
    AppConfig cfg = loadConfig("config/config.yaml");

    std::cout << "=====================================\n";
    std::cout << " Edge IoT Monitor Backend Starting...\n";
    std::cout << "=====================================\n";

    // 1. 初始化存储层
    std::cout << "[INIT] StorageEngine...\n";
    StorageEngine storage(cfg.db_path);

    if (!storage.init())
    {
        std::cerr << "[ERROR] Database init failed\n";
        return 1;
    }
    std::cout << "[OK] Database ready (" << cfg.db_path << ")\n";

    // 2. 初始化过滤器
    std::cout << "[INIT] DataFilter...\n";
    DataFilter filter(cfg.filter_window_seconds, cfg.filter_iqr_multiplier, cfg.filter_min_samples);
    std::cout << "[OK] Filter ready (window=" << cfg.filter_window_seconds
              << "s, IQR=" << cfg.filter_iqr_multiplier
              << ", min_samples=" << cfg.filter_min_samples << ")\n";

    // 3. 初始化 AI 调度器（云端 DeepSeek 主，本地 Ollama 备）
    std::cout << "[INIT] AIQueryDispatcher...\n";
    DeepSeekConfig deepseek;
    deepseek.enabled = cfg.deepseek_enabled;
    deepseek.base_url = cfg.deepseek_base_url;
    deepseek.model = cfg.deepseek_model;
    deepseek.api_key = cfg.deepseek_api_key;
    deepseek.timeout_s = cfg.deepseek_timeout_s;

    const bool deepseek_active = deepseek.enabled && !deepseek.api_key.empty();
    AIQueryDispatcher ai(
        storage,
        cfg.ollamaUrl(),
        cfg.ollama_model,
        cfg.ai_trigger_count,
        cfg.ai_window_size,
        deepseek,
        cfg.ai_enabled
    );
    std::cout << "[OK] AI ready (primary="
              << (deepseek_active ? "deepseek:" + cfg.deepseek_model
                                  : "ollama:" + cfg.ollama_model)
              << ", fallback=ollama:" << cfg.ollama_model
              << ", trigger=" << cfg.ai_trigger_count
              << ", window=" << cfg.ai_window_size
              << (cfg.ai_enabled ? "" : ", DISABLED") << ")\n";
    if (cfg.deepseek_enabled && cfg.deepseek_api_key.empty())
        std::cout << "[WARN] deepseek.enabled=true but no api_key "
                     "(set deepseek.api_key or DEEPSEEK_API_KEY) — using Ollama only\n";

    // 4. 初始化云端同步（占位 — 注册后填充 endpoint/credential）
    std::cout << "[INIT] CloudSync...\n";
    CloudSync cloud(
        storage,
        cfg.cloud_endpoint,
        cfg.cloud_project_id,
        cfg.cloud_device_id,
        cfg.cloud_credential
    );
    std::cout << "[OK] CloudSync ready"
              << (cloud.isEnabled() ? " (ENABLED)" : " (disabled — set cloud.enabled in config.yaml)")
              << "\n";

    // 5. 初始化数据接入层
    std::cout << "[INIT] DataIngestor...\n";
    DataIngestor ingestor(storage, filter, ai, cloud,
                           cfg.temp_min, cfg.temp_max,
                           cfg.hum_min,  cfg.hum_max,
                           cfg.device_allowlist);

    std::cout << "[OK] HTTP Server starting at http://" << cfg.server_host
              << ":" << cfg.server_port << "\n";
    std::cout << "[INFO] Press Ctrl+C to stop\n";

    // 关键点：start() 是阻塞的
    ingestor.start(cfg.server_port);

    // （理论上走不到这里，除非你实现了 stop）
    std::cout << "[SHUTDOWN] Server stopped\n";

    return 0;
}