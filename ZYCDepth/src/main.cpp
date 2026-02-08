#include "Thread/SystemManager.h"
#include <iostream>

// 处理 Ctrl+C 信号 防止卡死退出用
#include <csignal>
void signalHandler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received.\n";
    SystemManager::getInstance().stop();
}

int main() {
    // 注册信号处理
    signal(SIGINT, signalHandler);

    auto& sys = SystemManager::getInstance();
    sys.init();
    sys.start();



    // 4. 阻塞主线程，直到程序结束
    sys.runWait();

    return 0;
}