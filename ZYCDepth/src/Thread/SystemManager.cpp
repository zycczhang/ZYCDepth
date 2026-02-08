#include "SystemManager.h"
#include <iostream>
#include <chrono>
#include "WebSocket/WebSocketServer.h"
#include"Inference/DepthInference.h"
#include"UIManager/UIManager.h"
SystemManager& SystemManager::getInstance() {
    static SystemManager instance;
    return instance;
}

SystemManager::SystemManager() {
    
}

SystemManager::~SystemManager() {
    stop();
}

void SystemManager::init() {
    // 设置控制台UTF-8 编码输出，确保中文不乱吗
    SetConsoleOutputCP(CP_UTF8);
    Logger::getInstance().printLogo();
    // Logger 已经在其构造函数中启动了写文件线程，这里只需要配置回调
    LOG_INFO("系统初始化中",true);

    if (!UIManager::getInstance().init(1280, 720, L"ZYC")) {
        LOG_ERR("UI界面 初始化失败！", true);
    }
    // 实例化 WebSocket Server
    webServer = std::make_unique<WebSocketServer>(9001);

    // 2. 配置日志回调：将日志推送到 ImGui
    Logger::getInstance().setWebCallback([this](const LogEntry& entry) {
        // 将日志存入 UIManager 的日志缓冲区（下文会提到如何实现）
        UIManager::getInstance().addLog(entry.message, (int)entry.level);
        // 如果 webServer 还在，继续广播
        if (webServer) {
            nlohmann::json j;
            j["type"] = "log"; j["level"] = (int)entry.level;
            j["msg"] = entry.message; j["time"] = entry.timestamp;
            webServer->broadcastText(j.dump());
        }
        });


}

void SystemManager::start() {
    if (isRunning) return;
    isRunning = true;
    LOG_INFO("System starting threads...");

    // 放入线程池管理，this 指针用于访问成员
    threadPool.emplace_back(&SystemManager::captureThreadWorker, this);
    LOG_INFO("Capture thread launched.");

    //threadPool.emplace_back(&SystemManager::webServerThreadWorker, this);
    //LOG_INFO("WebServer thread launched.");
    //threadPool.emplace_back(&SystemManager::webBroadcastThreadWorker, this);
    //LOG_INFO("Web Broadcast thread launched.");

    threadPool.emplace_back(&SystemManager::depthInferenceThreadWorker, this);
    LOG_INFO("depthInference thread launched.");

    // 3. 启动建图/AI 线程 (预留)
    // threadPool.emplace_back(&SystemManager::mappingThreadWorker, this);
    // LOG_INFO("Mapping thread launched.");

}

void SystemManager::stop() {
    if (!isRunning) return;
    LOG_INFO("系统正在关闭...", true);
    isRunning = false;
    SharedContext::getInstance().setIsMapping(false);
    SharedContext::getInstance().setIsInferencing(false);
    // 等待后台线程结束
    for (auto& t : threadPool) {
        if (t.joinable()) t.join();
    }
    threadPool.clear();
    // 清理 UI 资源
    UIManager::getInstance().shutdown();

    LOG_INFO("所有模块已安全退出.", true);
}

void SystemManager::runWait() {
    // 【关键修改】主线程不再 sleep，而是直接运行 UI 循环
    // 这个函数会阻塞在这里，直到用户关闭 ImGui 窗口
    UIManager::getInstance().run();
    // 窗口关闭后，通知后台线程停止运行
    stop();
}

// ==========================================
// 线程工作函数具体实现
// ==========================================

void SystemManager::captureThreadWorker() {
    LOG_INFO("Capture Worker: Started.");
    ScreenGrabber grabber;
    long long frameID = 0;

    while (isRunning) {
        // 1. 获取当前配置的频率
        auto config = SharedContext::getInstance().getCurrentCaptureConfig();
        int targetFps = (config.captureFps > 0) ? config.captureFps : 30;
        int targetIntervalMs = 1000 / targetFps;

        // 2. 统计耗时并截图
        auto startTime = std::chrono::high_resolution_clock::now();

        auto matPtr = grabber.grab();

        auto endTime = std::chrono::high_resolution_clock::now();
        double durationMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        if (matPtr && !matPtr->empty()) {
            FrameData frame;
            frame.image = matPtr;
            frame.sequenceID = ++frameID;
            frame.timestamp = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            frame.captureDurationMs = durationMs; // 保存耗时
            SharedContext::getInstance().setCurrentFrame(std::move(frame));
            SharedContext::getInstance().setCaptureTime(durationMs); // 新增
            SharedContext::getInstance().setCurrentFrame(std::move(frame));
        }

        // 4. 动态计算睡眠时间，保证频率稳定
        auto workTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - startTime).count();
        long long sleepTime = targetIntervalMs - workTime;

        if (sleepTime > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
        }
    }
    LOG_INFO("Capture Worker: Exiting.");
}

void SystemManager::webServerThreadWorker() {
    LOG_INFO("Web Server Thread: Started.");

    // 启动 uWS 循环（这会阻塞当前线程）
    if (webServer) {
        webServer->run();
    }

    LOG_INFO("Web Server Thread: Exiting.");
}

void SystemManager::depthInferenceThreadWorker() {
    LOG_INFO("depthInference Worker: Started. Initializing AI Model...");

    // 初始化推理引擎 (此处可选 ONNX 或将来扩展 TensorRT)
    auto engine = std::make_unique<ONNXDepthInference>();
    if (!engine->init("models/DA3-SMALL-504.onnx")) {
        LOG_ERR("AI Model Init Failed!");
        return;
    }
    LOG_INFO("AI Model Loaded Successfully.");

    long long lastProcessedID = -1;

    while (isRunning) {
        if (!SharedContext::getInstance().getIsInferencing()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        FrameData frame = SharedContext::getInstance().waitForNewFrame(lastProcessedID);
        if (frame.empty()) continue;
        // 3. 执行 AI 推理
        DepthResult result = engine->predict(*frame.image);
        SharedContext::getInstance().setInferenceTime(result.inferTimeMs); // 新增
        lastProcessedID = frame.sequenceID;
        // 4. 封装完整结果
        FrameData depthFrame;
        // 仍然保留可视化图用于网页端 2D 预览
        depthFrame.image = std::make_shared<cv::Mat>(result.visualDepth);
        // 保存原始 float 深度图和矩阵用于 3D 还原
        depthFrame.rawDepth = std::make_shared<cv::Mat>(result.depthMap);
        depthFrame.intrinsics = result.intrinsics;
        depthFrame.extrinsics = result.extrinsics;

        depthFrame.sequenceID = frame.sequenceID;
        depthFrame.captureDurationMs = result.inferTimeMs;
        SharedContext::getInstance().setCurrentDepthFrame(std::move(depthFrame));
    }
    LOG_INFO("depthInference Worker: Exiting.");
}

// 由此线程专门负责将数据发送给网页渲染显示
void SystemManager::webBroadcastThreadWorker() {
    LOG_INFO("Web Broadcast Worker: Started.");
    long long lastRawID = -1;
    long long lastDepthID = -1;
    while (isRunning) {
        // 1. 广播原始游戏画面 (Base64 JSON，用于网页左侧预览)
        FrameData rawFrame = SharedContext::getInstance().getCurrentFrame();
        if (!rawFrame.empty() && rawFrame.sequenceID > lastRawID) {
            webServer->broadcastImage("raw", *rawFrame.image, rawFrame.captureDurationMs);
            lastRawID = rawFrame.sequenceID;
        }
        // 2. 广播深度图
        FrameData depthFrame = SharedContext::getInstance().getCurrentDepthFrame();
        if (!depthFrame.empty() && depthFrame.sequenceID > lastDepthID) {
            // A. 发送可视化图片 (Base64 JSON，用于网页右侧预览)
            webServer->broadcastImage("depth", *depthFrame.image, depthFrame.captureDurationMs);

            // B. 发送二进制深度数据 (用于网页 3D 点云还原)
            webServer->broadcastDepthBinary(depthFrame);

            lastDepthID = depthFrame.sequenceID;
        }
        // 保持 30fps 的检查频率
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}
