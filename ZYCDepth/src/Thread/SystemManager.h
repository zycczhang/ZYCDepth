#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

// 引入你的组件
#include "ScreenGrabber/ScreenGrabber.h"
#include "Log/Logger.h"
#include "Data/CommonTypes.h"

// 前向声明 WebSocket 服务（暂时还没写，留个位置）
class WebSocketServer;

class SystemManager {
public:
    // 单例模式：通常 SystemManager 全局唯一
    static SystemManager& getInstance();


    // 删除拷贝构造
    SystemManager(const SystemManager&) = delete;
    SystemManager& operator=(const SystemManager&) = delete;

    /**
     * @brief 系统初始化
     * @details 配置日志回调、加载配置文件等准备工作
     */
    void init();

    /**
     * @brief 启动系统
     * @details 拉起所有工作线程
     */
    void start();

    /**
     * @brief 停止系统
     * @details 发送停止信号，等待所有线程安全退出
     */
    void stop();

    /**
     * @brief 运行主循环（阻塞主线程）
     * @details 防止 main 函数直接退出
     */
    void runWait();

private:
    SystemManager();
    ~SystemManager();

    // 线程工作函数
    void captureThreadWorker();    // 截图线程逻辑
    void mappingThreadWorker();    // 建图/AI线程逻辑（预留）
    void webServerThreadWorker();  // Web服务器线程逻辑

    void depthInferenceThreadWorker();

    void webBroadcastThreadWorker();

private:
    std::atomic<bool> isRunning{ false }; // 全局运行标志
    std::vector<std::thread> threadPool;  // 线程池管理所有线程句柄

    // 子模块实例
    std::unique_ptr<ScreenGrabber> screenGrabber;
    std::unique_ptr<WebSocketServer> webServer;
    // std::unique_ptr<WebSocketServer> webServer; // 待实现
};