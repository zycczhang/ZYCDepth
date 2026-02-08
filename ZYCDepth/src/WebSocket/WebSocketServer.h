#pragma once
#include <uwebsockets/App.h>
#include <nlohmann/json.hpp>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "Data/CommonTypes.h"

using json = nlohmann::json;

class WebSocketServer {
public:
    // 定义 WebSocket 连接的数据结构（每个连接可以有自己的状态）
    struct PerSocketData {
        /* 如果需要记录特定客户端信息可以写在这里 */
    };

    WebSocketServer(int port = 9001);
    ~WebSocketServer();

    // 启动服务器（会阻塞，需要在独立线程运行）
    void run();
    // 停止服务器
    void stop();

    // 广播文本消息（如日志、状态更新）
    void broadcastText(const std::string& message);

    // 发送图像帧（二进制或Base64）
    void broadcastImage(const std::string& type, const cv::Mat& frame, double durationMs = 0.0);

    void broadcastDepthBinary(const FrameData& fd);

private:
    int port;
    struct us_listen_socket_t* listen_socket = nullptr;
    uWS::Loop* loop = nullptr;

    // 记录所有活跃的 WebSocket 实例，用于手动推送数据
    std::mutex mtx;
    std::set<uWS::WebSocket<false, true, PerSocketData>*> sockets;

    // 处理来自 Web 端的消息
    void handleMessage(std::string_view message, uWS::WebSocket<false, true, PerSocketData>* ws);
};


