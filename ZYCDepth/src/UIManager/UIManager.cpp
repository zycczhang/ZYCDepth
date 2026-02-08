#include "UIManager.h"
#include "Thread/SystemManager.h"
#include "Data/CommonTypes.h"
#include "Log/Logger.h"
#include <algorithm> // 必须包含这个
#include <iostream>

// 导入 ImGui 内部 Win32 处理函数
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- Win32 窗口回调函数 ---
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && UIManager::getInstance().getDevice() != nullptr) {
            UIManager::getInstance().requestResize((UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0; // 禁用 ALT 菜单
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- UIManager 实现 ---

void UIManager::addLog(const std::string& msg, int level)
{
    std::lock_guard<std::mutex> lock(logMtx);
    guiLogs.push_back({ msg, level });
    if (guiLogs.size() > 100) guiLogs.erase(guiLogs.begin()); // 保持 100 条
}

bool UIManager::init(int width, int height, const wchar_t* title) {
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    g_wc = { sizeof(g_wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ZYC_AI_UI", nullptr };
    ::RegisterClassExW(&g_wc);

    g_hwnd = ::CreateWindowW(g_wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
        100, 100, (int)(width * main_scale), (int)(height * main_scale),
        nullptr, nullptr, g_wc.hInstance, nullptr);

    if (!g_hwnd) return false;

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
        return false;
    }

    ::ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    // 路径可以是 C:\\Windows\\Fonts\\msyh.ttc，建议将字体文件放在程序目录下
    const char* font_path = "C:\\Windows\\Fonts\\msyh.ttc";
    // 18.0f 是字体大小，GetGlyphRangesChineseFull() 是关键
    io.Fonts->AddFontFromFileTTF(font_path, 18.0f, NULL, io.Fonts->GetGlyphRangesChineseFull());

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
    style.ScaleAllSizes(main_scale);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    LOG_INFO("UI Manager 初始化完成", true);
    return true;
}

void UIManager::run() {
    ImGuiIO& io = ImGui::GetIO();
    bool done = false;

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            handleResize();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();



        updateUI();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());


        g_pSwapChain->Present(1, 0);
    }
}

void UIManager::setupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(10, 10);
    // 设置暗黑主题（类似图片）
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.16f, 1.00f);

    // 绿色荧光高亮
    ImVec4 limeGreen = ImVec4(0.50f, 0.80f, 0.00f, 1.00f);
    colors[ImGuiCol_CheckMark] = limeGreen;
    colors[ImGuiCol_SliderGrab] = limeGreen;
    colors[ImGuiCol_SliderGrabActive] = limeGreen;
    colors[ImGuiCol_ButtonActive] = limeGreen;
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
}
void UIManager::refreshWindowList() {
    windowList.clear();
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (IsWindowVisible(hwnd) && GetWindowTextLength(hwnd) > 0) {
            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            std::string utf8Title = SharedContext::GbkToUtf8(title); // 使用你之前的转换函数
            if (!utf8Title.empty() && utf8Title != "Program Manager") {
                reinterpret_cast<std::vector<std::string>*>(lParam)->push_back(utf8Title);
            }
        }
        return TRUE;
        }, reinterpret_cast<LPARAM>(&windowList));
}

// 辅助函数：计算图片在给定容器内的等比例缩放大小
ImVec2 CalcMaxFillSize(float imgW, float imgH, ImVec2 containerSize) {
    if (imgW <= 0 || imgH <= 0 || containerSize.x <= 0 || containerSize.y <= 0)
        return ImVec2(0, 0);
    // 使用标准库的 std::min
    float scale = std::min(containerSize.x / imgW, containerSize.y / imgH);

    return ImVec2(imgW * scale, imgH * scale);
}

void UIManager::updateUI() {
    setupStyle();

    // 全屏铺满窗口
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Pos);
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
    ImGui::Begin("MainShell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ==========================================
    // 1. 顶部状态栏 (Status + Window Selector)
    // ==========================================
    ImGui::BeginChild("TopBar", ImVec2(0, 50), true);
    ImGui::SetCursorPosY(12);
    ImGui::Text("ZYC AI SYSTEM");
    ImGui::SameLine(ImGui::GetWindowWidth() - 400); // 靠右对齐下拉框

    ImGui::EndChild();

    // 计算中间区域的高度 (总高度 - 顶部 - 底部日志区)
    float contentHeight = ImGui::GetContentRegionAvail().y - 220;

    // ==========================================
    // 2. 中间三列布局
    // ==========================================
    // --- 第一列：左侧控制台 ---
    ImGui::BeginChild("LeftConsole", ImVec2(300, contentHeight), true);
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.0f, 1.0f), "SYSTEM CONTROL");
    ImGui::Separator();
    // --- 新增：性能监控仪表盘 ---
    ImGui::BeginChild("PerfMetrics", ImVec2(0, 80), true);
    {
        double capTime = SharedContext::getInstance().getCaptureTime();
        double infTime = SharedContext::getInstance().getInferenceTime();
        ImGui::Columns(2, "perf_cols", false);
        ImGui::Text("截图耗时");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%.1f ms", capTime);
        ImGui::NextColumn();
        ImGui::Text("推理耗时");
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%.1f ms", infTime);
        ImGui::Columns(1);
    }
    ImGui::EndChild();
    ImGui::Spacing();

    auto config = SharedContext::getInstance().getCurrentCaptureConfig();
    bool changed = false;
    const char* methods[] = { "GDI", "DirectX", "WinGC" };
    int current_method = (int)config.Method;
    const char* preview = (selectedWindowIdx == -1) ? "选择游戏窗口..." : windowList[selectedWindowIdx].c_str();
    if (ImGui::BeginCombo("##TargetWindow", preview)) {
        if (ImGui::Button("刷新列表")) refreshWindowList();
        for (int n = 0; n < (int)windowList.size(); n++) {
            if (ImGui::Selectable(windowList[n].c_str(), selectedWindowIdx == n)) {
                selectedWindowIdx = n;
                auto config = SharedContext::getInstance().getCurrentCaptureConfig();
                config.targetWindowName = windowList[n];
                SharedContext::getInstance().setCurrentCaptureConfig(config);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Combo("Capture Tech", &current_method, methods, 3)) {
        config.Method = (CaptureMethod)current_method;
        changed = true;
    }
    if (ImGui::SliderInt("FPS Limit", &config.captureFps, 1, 60)) changed = true;

    bool isInfer = SharedContext::getInstance().getIsInferencing();
    if (ImGui::Checkbox("Inference Active", &isInfer)) SharedContext::getInstance().setIsInferencing(isInfer);

    if (changed) SharedContext::getInstance().setCurrentCaptureConfig(config);

    // 这里可以放你寻路算法的参数调优
    ImGui::Separator();
    ImGui::Text("Pathfinding Params");
    static float voxelSize = 0.1f;
    ImGui::DragFloat("Voxel Size", &voxelSize, 0.01f, 0.01f, 1.0f);

    ImGui::EndChild();

    ImGui::SameLine();

    // --- 第二列：中间图像显示 (原图 + 深度图垂直排列) ---
    ImGui::BeginChild("MiddleFeeds", ImVec2(400, contentHeight), false);
    {
        // 1. 原图预览 (RAW FEED)
        // 关键点：增加 ImGuiWindowFlags_NoScrollbar 标志，彻底禁用滚动条
        ImGui::BeginChild("RawView", ImVec2(0, contentHeight * 0.5f - 5), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::Text("RAW FEED");

        auto raw = SharedContext::getInstance().getCurrentFrame();
        if (!raw.empty()) {
            ImTextureID tex = getTextureFromMat("raw_ui", *raw.image);

            // 获取当前子窗口剩余的可读区域大小
            ImVec2 availSize = ImGui::GetContentRegionAvail();

            // 计算自适应大小（保持原图比例，且不超出 availSize）
            ImVec2 displaySize = CalcMaxFillSize((float)raw.image->cols, (float)raw.image->rows, availSize);

            // 居中显示（可选）
            float offsetX = (availSize.x - displaySize.x) * 0.5f;
            float offsetY = (availSize.y - displaySize.y) * 0.5f;
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + offsetX, ImGui::GetCursorPosY() + offsetY));
            ImGui::Image(tex, displaySize);
        }
        ImGui::EndChild();
        // 2. 深度图预览 (AI DEPTH)
        ImGui::BeginChild("DepthView", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::Text("AI DEPTH");

        auto depth = SharedContext::getInstance().getCurrentDepthFrame();
        if (!depth.empty()) {
            ImTextureID tex = getTextureFromMat("depth_ui", *depth.image);

            ImVec2 availSize = ImGui::GetContentRegionAvail();
            // 同样的缩放算法
            ImVec2 displaySize = CalcMaxFillSize((float)depth.image->cols, (float)depth.image->rows, availSize);

            float offsetX = (availSize.x - displaySize.x) * 0.5f;
            float offsetY = (availSize.y - displaySize.y) * 0.5f;
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + offsetX, ImGui::GetCursorPosY() + offsetY));
            ImGui::Image(tex, displaySize);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- 第三列：右侧 3D 视图 ---
    ImGui::BeginChild("Right3DView", ImVec2(0, contentHeight), true);
    {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.0f, 1.0f), "3D RECONSTRUCTION");
        // 1. 获取当前子窗口的可读区域位置和大小
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();    // 这里的坐标是绝对屏幕坐标
        ImVec2 canvasSize = ImGui::GetContentRegionAvail(); // 剩余可用区域
        // 2. 绘制一个占位的透明按钮，用于捕获鼠标点击或滚动（可选）
        ImGui::InvisibleButton("3DCanvas", canvasSize);

        // 3. 【核心调用】：在这里执行你的 3D 点云渲染逻辑
        // 因为 renderPointCloud 内部使用了 GetWindowDrawList，
        // 它会直接在刚才绘制的 InvisibleButton 所在的层级进行绘图
        renderPointCloud(canvasPos, canvasSize);
    }
    ImGui::EndChild();

    // ==========================================
    // 3. 底部日志栏
    // ==========================================
    ImGui::Spacing();
    ImGui::BeginChild("BottomLogs", ImVec2(0, 0), true);
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.0f, 1.0f), "SYSTEM LOG");
    if (ImGui::BeginChild("LogScroll")) {
        std::lock_guard<std::mutex> lock(logMtx);
        for (auto& line : guiLogs) {
            ImGui::TextUnformatted(line.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::End();
}

void UIManager::renderPointCloud(ImVec2 canvasPos, ImVec2 canvasSize) {
    // 1. 数据获取
    auto depthFrame = SharedContext::getInstance().getCurrentDepthFrame();
    auto rawFrame = SharedContext::getInstance().getCurrentFrame();
    if (depthFrame.empty() || !depthFrame.rawDepth || rawFrame.empty()) return;
    // --- 2. 交互状态保存 (使用 static 保持状态) ---
    static float zoom = 810.0f;
    static float rotX = -0.25f;   // 初始俯视角度
    static float rotY = 0.36f;   // 初始左右角度
    static ImVec2 panOffset = ImVec2(0, 0); // 平移偏移
    // --- 3. 处理鼠标交互 ---
    ImGuiIO& io = ImGui::GetIO();
    // 检查鼠标是否在 3D 窗口区域内
    bool isHovered = ImGui::IsWindowHovered() || ImGui::IsItemHovered();

    if (isHovered) {
        // A. 滚轮缩放
        if (io.MouseWheel != 0) {
            zoom += io.MouseWheel * 20.0f;
            if (zoom < 10.0f) zoom = 10.0f; // 防止缩放太小
            //std::cout <<"zoom" << zoom << std::endl;
        }
        // B. 左键拖动：旋转镜头
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            rotY -= io.MouseDelta.x * 0.01f;
            rotX += io.MouseDelta.y * 0.01f;
            //std::cout <<"rotY" << rotY << std::endl;
            //std::cout <<"rotX" << rotX << std::endl;
        }
        // C. 右键或中键拖动：平移镜头
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            panOffset.x += io.MouseDelta.x;
            panOffset.y += io.MouseDelta.y;
        }
        // 双击左键重置视角
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            zoom = 810.0f;
            rotX = -0.25f;   // 初始俯视角度
            rotY = 0.36f;   // 初始左右角度; 
            panOffset = ImVec2(0, 0);
        }
    }
    // --- 4. 准备绘图 ---
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    cv::Mat& K = depthFrame.intrinsics;
    cv::Mat& Rt = depthFrame.extrinsics;
    cv::Mat& dMap = *depthFrame.rawDepth;
    cv::Mat& colorMap = *rawFrame.image;
    float fx = K.at<float>(0, 0), fy = K.at<float>(1, 1);
    float cx = K.at<float>(0, 2), cy = K.at<float>(1, 2);
    cv::Mat R = Rt(cv::Rect(0, 0, 3, 3));
    cv::Mat t = Rt(cv::Rect(3, 0, 1, 3));
    int step =1; // 采样步长（每几个像素采样一次）目前是每个像素都采样，可以增加步长提高性能
    float uvToColorX = (float)colorMap.cols / dMap.cols;
    float uvToColorY = (float)colorMap.rows / dMap.rows;
    // --- 5. 循环渲染点云 ---
    for (int v = 0; v < dMap.rows; v += step) {
        for (int u = 0; u < dMap.cols; u += step) {
            float z = dMap.at<float>(v, u);
            if (z <= 0.1f || z > 50.0f) continue;
            // A. 相机系 -> 世界系 (同前)
            float xc = (u - cx) * z / fx;
            float yc = (v - cy) * z / fy;
            float zc = z;
            float xw = R.at<float>(0, 0) * xc + R.at<float>(0, 1) * yc + R.at<float>(0, 2) * zc + t.at<float>(0, 0);
            float yw = R.at<float>(1, 0) * xc + R.at<float>(1, 1) * yc + R.at<float>(1, 2) * zc + t.at<float>(1, 0);
            float zw = R.at<float>(2, 0) * xc + R.at<float>(2, 1) * yc + R.at<float>(2, 2) * zc + t.at<float>(2, 0);
            // B. 坐标修正
            float xf = xw;
            float yf = yw;
            float zf = zw;
            // C. 旋转变换
            // 绕 Y 轴
            float rx = xf * cos(rotY) + zf * sin(rotY);
            float rz = -xf * sin(rotY) + zf * cos(rotY);
            // 绕 X 轴
            float ry = yf * cos(rotX) - rz * sin(rotX);

            // D. 最终投影到屏幕 (加上 panOffset)
            float screenX = canvasPos.x + (canvasSize.x * 0.5f) + rx * zoom + panOffset.x;
            float screenY = canvasPos.y + (canvasSize.y * 0.5f) + ry * zoom + panOffset.y;
            // E. 画布裁剪检查与取色绘制
            if (screenX > canvasPos.x && screenX < canvasPos.x + canvasSize.x &&
                screenY > canvasPos.y && screenY < canvasPos.y + canvasSize.y) {

                int cU = (int)(u * uvToColorX);
                int cV = (int)(v * uvToColorY);
                cv::Vec3b bgr = colorMap.at<cv::Vec3b>(std::min(cV, colorMap.rows - 1), std::min(cU, colorMap.cols - 1));

                drawList->AddRectFilled(
                    ImVec2(screenX, screenY),
                    ImVec2(screenX + 1.5f, screenY + 1.5f),
                    IM_COL32(bgr[2], bgr[1], bgr[0], 255)
                );
            }
        }
    }
}

ImTextureID UIManager::getTextureFromMat(const std::string& name, const cv::Mat& mat) {
    if (mat.empty()) return ImTextureID(0);
    auto& res = textureCache[name];
    // 1. 如果尺寸或格式改变，释放旧资源
    if (res.srv && (res.width != mat.cols || res.height != mat.rows)) {
        res.srv->Release(); res.srv = nullptr;
        res.texture->Release(); res.texture = nullptr;
    }
    // 2. 创建资源 (DXGI_FORMAT_B8G8R8A8_UNORM 是最匹配 OpenCV 的)
    if (!res.srv) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = mat.cols;
        desc.Height = mat.rows;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &res.texture))) return ImTextureID(0);
        if (FAILED(g_pd3dDevice->CreateShaderResourceView(res.texture, nullptr, &res.srv))) return ImTextureID(0);
        res.width = mat.cols;
        res.height = mat.rows;
    }
    // 3. 准备 BGRA 数据
    cv::Mat bgra;
    if (mat.channels() == 3) cv::cvtColor(mat, bgra, cv::COLOR_BGR2BGRA);
    else if (mat.channels() == 1) cv::cvtColor(mat, bgra, cv::COLOR_GRAY2BGRA);
    else bgra = mat;
    // 4. 拷贝到 GPU
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(res.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        uint8_t* pDest = (uint8_t*)mapped.pData;
        uint8_t* pSrc = bgra.data;
        for (int y = 0; y < res.height; y++) {
            memcpy(pDest + y * mapped.RowPitch, pSrc + y * bgra.step, res.width * 4);
        }
        g_pd3dDeviceContext->Unmap(res.texture, 0);
    }
    return (ImTextureID)res.srv;
}

// --- D3D11 内部辅助代码 ---

bool UIManager::CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext)))
        return false;

    CreateRenderTarget();
    return true;
}

void UIManager::CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void UIManager::CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void UIManager::handleResize() {
    CleanupRenderTarget();
    g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
    g_ResizeWidth = g_ResizeHeight = 0;
    CreateRenderTarget();
}

void UIManager::shutdown() {
    for (auto& pair : textureCache) {
        if (pair.second.srv) pair.second.srv->Release();
        if (pair.second.texture) pair.second.texture->Release(); // 释放纹理资源
    }
    textureCache.clear();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(g_hwnd);
    ::UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
}

void UIManager::CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pd3dDeviceContext) g_pd3dDeviceContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();
}