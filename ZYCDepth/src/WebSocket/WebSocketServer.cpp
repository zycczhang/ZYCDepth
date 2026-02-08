#include "WebSocketServer.h"
#include "Log/Logger.h"
#include <opencv2/imgcodecs.hpp>
#include<Data/CommonTypes.h>


// Base64 编码辅助（发送图像给 Web 最简单的方法）
static std::string base64_encode(const std::vector<uchar>& data) {
    static const char* codes = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uchar c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(codes[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(codes[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

WebSocketServer::WebSocketServer(int port) : port(port) {}

WebSocketServer::~WebSocketServer() { stop(); }

void WebSocketServer::run() {
    uWS::App().ws<PerSocketData>("/*", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 16 * 1024 * 1024, // 16MB 足够传大图
        .open = [this](auto* ws) {
            std::lock_guard<std::mutex> lock(mtx);
            sockets.insert(ws);
            LOG_INFO("网页已连接. Total: " + std::to_string(sockets.size()),true);

            // --- 新增：推送当前配置 ---
            CaptureConfig current = SharedContext::getInstance().getCurrentCaptureConfig();
            nlohmann::json j;
            j["type"] = "init_config";
            j["method"] = (int)current.Method;
            j["window_name"] = current.targetWindowName;
            j["capturefps"] = current.captureFps;
            // 发送给刚连接的这个客户端
            ws->send(j.dump(), uWS::OpCode::TEXT);

        },
        .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
            handleMessage(message, ws);
        },
        .close = [this](auto* ws, int code, std::string_view message) {
            std::lock_guard<std::mutex> lock(mtx);
            sockets.erase(ws);
            LOG_INFO("网页断开.", true);
        }
        }).listen(port, [this](auto* listen_socket) {
            if (listen_socket) {
                this->listen_socket = listen_socket;
                LOG_INFO("websocket port " + std::to_string(port));
            }
            }).run();
}

void WebSocketServer::stop() {
    if (listen_socket) {
        us_listen_socket_close(0, listen_socket);
        listen_socket = nullptr;
    }
}

void WebSocketServer::handleMessage(std::string_view message, uWS::WebSocket<false, true, PerSocketData>* ws) {
    try {
        auto j = json::parse(message);
        std::string type = j.value("type", "");

        if (type == "set_capture_config") {
            CaptureConfig config = SharedContext::getInstance().getCurrentCaptureConfig();
            // 检查字段是否存在，防止解析异常
            if (j.contains("window_name")) {
                config.targetWindowName = j["window_name"];
            }
            if (j.contains("method")) {
                // 将前端传来的数字转为 CaptureMethod 枚举
                config.Method = static_cast<CaptureMethod>(j["method"].get<int>());
            }
            if (j.contains("capture_fps")) {
                config.captureFps = j["capture_fps"].get<int>();
            }
            SharedContext::getInstance().setCurrentCaptureConfig(config);
            
        }

        else if (type == "toggle_mapping") {
            bool start = j.value("state", false);
            SharedContext::getInstance().setIsMapping(start);
            LOG_INFO(start ? "Mapping started" : "Mapping stopped");
        }

        else if (type == "toggle_Inference") {
            bool start = j.value("state", false);
            SharedContext::getInstance().setIsInferencing(start);
            LOG_INFO(start ? "推理开始" : "推理结束 ");
        }

        else if (type == "get_window_list") {
            std::vector<std::string> windowList;
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                if (IsWindowVisible(hwnd) && GetWindowTextLength(hwnd) > 0) {
                    char title[256];
                    GetWindowTextA(hwnd, title, sizeof(title));

                    std::string utf8Title = SharedContext::GbkToUtf8(title); // 【关键：转换编码】

                    if (!utf8Title.empty() && utf8Title != "Program Manager") {
                        auto* list = reinterpret_cast<std::vector<std::string>*>(lParam);
                        list->push_back(utf8Title);
                    }
                }
                return TRUE;
                }, reinterpret_cast<LPARAM>(&windowList));

            nlohmann::json response;
            response["type"] = "window_list";
            response["windows"] = windowList; // 此时 windowList 里的全是 UTF-8，解析器不再报错
            broadcastText(response.dump());
        }
    }
    catch (const std::exception& e) {
        // 打印具体的异常信息，方便定位是哪个字段错了
        LOG_ERR("JSON Parse Error: " + std::string(e.what()));
    }
}

void WebSocketServer::broadcastText(const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto* ws : sockets) {
        ws->send(message, uWS::OpCode::TEXT);
    }
}

void WebSocketServer::broadcastImage(const std::string& type, const cv::Mat& frame, double durationMs) {
    if (frame.empty()) return;

    // 1. 编码为 JPG (减少带宽)
    std::vector<uchar> buf;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 70 };
    cv::imencode(".jpg", frame, buf, params);

    // 2. 构造 JSON 协议
    json j;
    j["type"] = "frame_update";
    j["frame_type"] = type;
    j["data"] = "data:image/jpeg;base64," + base64_encode(buf);

    // 根据类型区分字段，或者 JS 端统一处理
    if (type == "raw") j["capture_time"] = durationMs;
    else if (type == "depth") j["infer_time"] = durationMs; // 确保 JS 能拿到这个 key
    broadcastText(j.dump());
}

void WebSocketServer::broadcastDepthBinary(const FrameData& fd) {
    if (fd.rawDepth->empty()) return;
    // 1. 定义二进制协议头 (确保字节对齐)
#pragma pack(push, 1)
    struct DepthHeader {
        uint32_t magic = 0xDEADBEEF; // 校验码
        int32_t width;
        int32_t height;
        float intrinsics[9];  // fx, 0, cx, 0, fy, cy, 0, 0, 1
        float extrinsics[12]; // 3x4 R|t 矩阵
    } header;
#pragma pack(pop)
    header.width = fd.rawDepth->cols;
    header.height = fd.rawDepth->rows;

    // 拷贝矩阵数据
    std::memcpy(header.intrinsics, fd.intrinsics.data, 9 * sizeof(float));
    std::memcpy(header.extrinsics, fd.extrinsics.data, 12 * sizeof(float));
    // 2. 拼接 Buffer
    size_t depthSize = fd.rawDepth->total() * sizeof(float);
    std::vector<char> packet(sizeof(header) + depthSize);

    std::memcpy(packet.data(), &header, sizeof(header));
    std::memcpy(packet.data() + sizeof(header), fd.rawDepth->data, depthSize);
    // 3. 发送
    std::lock_guard<std::mutex> lock(mtx);
    for (auto* ws : sockets) {
        ws->send(std::string_view(packet.data(), packet.size()), uWS::OpCode::BINARY);
    }
}