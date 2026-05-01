#include "StorageEngine.h"
#include "DataFilter.h"
#include "AIQueryDispatcher.h"
#include "DataIngestor.h"

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

    std::cout << "=====================================\n";
    std::cout << " Edge IoT Monitor Backend Starting...\n";
    std::cout << "=====================================\n";

    // 1. 初始化存储层
    std::cout << "[INIT] StorageEngine...\n";
    StorageEngine storage("sensor.db");

    if (!storage.init())
    {
        std::cerr << "[ERROR] Database init failed\n";
        return 1;
    }
    std::cout << "[OK] Database ready\n";

    // 2. 初始化过滤器
    std::cout << "[INIT] DataFilter...\n";
    DataFilter filter(60, 1.5);
    std::cout << "[OK] Filter ready (window=60, IQR=1.5)\n";

    // 3. 初始化 AI 调度器
    std::cout << "[INIT] AIQueryDispatcher...\n";
    AIQueryDispatcher ai(
        storage,
        "http://localhost:11434",     // Ollama
        "qwen2.5:3b", // 模型
        20,                           // trigger interval
        10                            // analysis window
    );
    std::cout << "[OK] AI ready\n";

    // 4.初始化数据接入层
    std::cout << "[INIT] DataIngestor...\n";
    DataIngestor ingestor(storage, filter, ai);

    std::cout << "[OK] HTTP Server starting at http://localhost:8080\n";
    std::cout << "[INFO] Press Ctrl+C to stop\n";

    // 关键点：start() 是阻塞的
    ingestor.start(8080);

    // （理论上走不到这里，除非你实现了 stop）
    std::cout << "[SHUTDOWN] Server stopped\n";

    return 0;
}