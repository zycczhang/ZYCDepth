#pragma once
#include "Data/CommonTypes.h"
#include <windows.h>
#include <memory>
#include <vector>

#include <inspectable.h>
#include <dwmapi.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <d3d11.h>
#include <wrl.h>

// 抽象截图策略接口
class ICaptureStrategy {
public:
    virtual ~ICaptureStrategy() = default;
    // 返回 true 表示截图成功，图像存入 result
    virtual bool capture(HWND hwnd, cv::Mat& result) = 0;
    // 清理资源（当窗口大小改变或策略切换时调用）
    virtual void cleanup() = 0;
};

// GDI 截图策略具体实现
class GDICaptureStrategy : public ICaptureStrategy {
private:
    HDC hScreenDC = nullptr;   // 屏幕设备上下文
    HDC hMemoryDC = nullptr;   // 内存设备上下文
    HBITMAP hBitmap = nullptr; // 位图句柄
    HBITMAP hOldBitmap = nullptr;
    int cachedWidth = 0;
    int cachedHeight = 0;
    

public:
    ~GDICaptureStrategy();
    
    bool capture(HWND hwnd, cv::Mat& result) override;
    void cleanup() override;
};

// WinGC 截图策略
class WinGCCaptureStrategy : public ICaptureStrategy {
private:
    // WinRT 相关对象
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{ nullptr };

    // D3D11 相关（用于 CPU 访问）
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
    HWND currentHwnd = nullptr;
    int width = 0;
    int height = 0;
    bool initWinGC(HWND hwnd);
    void copyTextureToMat(const Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture, cv::Mat& mat);
public:
    WinGCCaptureStrategy();
    ~WinGCCaptureStrategy() override;
    bool capture(HWND hwnd, cv::Mat& result) override;
    void cleanup() override;
};

// 截图器主类
class ScreenGrabber {
private:
    std::unique_ptr<ICaptureStrategy> strategy; // 当前策略实例
    CaptureConfig activeConfig; // 内部记录当前正在使用的配置
    uint64_t localConfigVersion = 0; // 本地记录的版本号
    void setConfig(const CaptureConfig& config);
public:
    ScreenGrabber();
    ~ScreenGrabber();
    /**
     * @brief 执行一次截图
     * @return std::shared_ptr<cv::Mat> 成功返回图像，失败返回 nullptr
     */
    std::shared_ptr<cv::Mat> grab();
};
