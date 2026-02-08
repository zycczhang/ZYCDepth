#pragma once
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <opencv2/opencv.hpp>
#include <map>
#include <string>
#include <vector>
#include <mutex>

class UIManager {
public:
    static UIManager& getInstance() {
        static UIManager instance;
        return instance;
    }

    struct LogLine { std::string text; int level; };
    std::vector<LogLine> guiLogs;
    std::mutex logMtx;

    void addLog(const std::string& msg, int level);
    bool init(int width, int height, const wchar_t* title);
    void shutdown();
    void run();

    // 基础功能接口
    void requestResize(UINT width, UINT height) { g_ResizeWidth = width; g_ResizeHeight = height; }
    ID3D11Device* getDevice() { return g_pd3dDevice; }
    ImTextureID getTextureFromMat(const std::string& name, const cv::Mat& mat);

private:
    UIManager() = default;
    void updateUI();
    void renderPointCloud(ImVec2 canvasPos, ImVec2 canvasSize);
    void setupStyle(); // 设置类似图片的暗黑+绿荧光主题
    void refreshWindowList(); // 刷新当前运行的游戏窗口列表

    // D3D11 绘图
    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void handleResize();

private:
    HWND g_hwnd = nullptr;
    WNDCLASSEXW g_wc = {};
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
    UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

    // 窗口列表数据
    std::vector<std::string> windowList;
    int selectedWindowIdx = -1;

    // 纹理缓存
    struct TextureResource {
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Texture2D* texture = nullptr;
        int width = 0;
        int height = 0;
    };
    std::map<std::string, TextureResource> textureCache;

    // UI 内部状态
    int activeTab = 0; // 侧边栏选中的索引
};