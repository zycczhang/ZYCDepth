#include "ScreenGrabber.h"
#include <iostream>
#include"Log/Logger.h"
#include <unknwn.h>
// WinRT 核心
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>
// WinRT Interop (处理 HWND 和 D3D11 交互)
#include <windows.graphics.capture.interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h> 
// 链接库
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d3d11.lib")


// ==========================================
// GDI 策略实现
// ==========================================

GDICaptureStrategy::~GDICaptureStrategy() {
    cleanup();
}

void GDICaptureStrategy::cleanup() {
    if (hOldBitmap && hMemoryDC) SelectObject(hMemoryDC, hOldBitmap);
    if (hBitmap) DeleteObject(hBitmap);
    if (hMemoryDC) DeleteDC(hMemoryDC);
    if (hScreenDC) ReleaseDC(nullptr, hScreenDC);

    hBitmap = nullptr;
    hMemoryDC = nullptr;
    hScreenDC = nullptr;
    cachedWidth = 0;
    cachedHeight = 0;
}

bool GDICaptureStrategy::capture(HWND hwnd, cv::Mat& result) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    // 1. 检查窗口是否最小化
    if (IsIconic(hwnd)) return false;
    // 1. 获取客户区大小
    RECT rect;
    if (!GetClientRect(hwnd, &rect)) return false;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (width <= 0 || height <= 0) return false;

    // 2. 如果尺寸变了或未初始化，重新创建资源
    if (width != cachedWidth || height != cachedHeight || !hMemoryDC) {
        cleanup(); // 清理旧资源

        hScreenDC = GetDC(hwnd);
        hMemoryDC = CreateCompatibleDC(hScreenDC);
        hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
        hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

        cachedWidth = width;
        cachedHeight = height;
    }

    // 3. 执行 BitBlt (像素拷贝: 显存 -> 内存DC)
    // SRCCOPY 直接拷贝，对于大多数游戏窗口适用
    //BOOL success = BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY);
    // BitBlt方法截图出来是黑屏，AI说是开启硬件加速了原因，不知道咋办，PrintWindow性能一般
    BOOL success = PrintWindow(hwnd, hMemoryDC, PW_RENDERFULLCONTENT);
    if (!success) return false;

    // 4. 将 GDI Bitmap 数据转入 OpenCV Mat
    // 创建BITMAPINFO结构来定义我们想要的数据格式
    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height; // 负高度表示自顶向下，正高度是倒立的
    bi.biPlanes = 1;
    bi.biBitCount = 32;    // GDI通常是32位 BGRA
    bi.biCompression = BI_RGB;

    // 准备 OpenCV Mat (预分配内存)
    // CV_8UC4 对应 BGRA
    result.create(height, width, CV_8UC4);

    // 获取位图数据到 Mat 的 data 指针中
    // 注意：GetDIBits 可能会比较耗时，这里是内存拷贝
    if (GetDIBits(hMemoryDC, hBitmap, 0, height, result.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {

        cv::Mat bgr;
        cv::cvtColor(result, bgr, cv::COLOR_BGRA2BGR);
        result = bgr;

        return true;
    }

    return false;
}



// 辅助：HWND 转 CaptureItem
inline auto CreateCaptureItemForWindow(HWND hwnd) {
    auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(factory->CreateForWindow(hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}
// 辅助：创建 Direct3D11Device
inline auto CreateDirect3DDevice(ID3D11Device* d3d11Device) {
    auto dxgiDevice = Microsoft::WRL::ComPtr<IDXGIDevice>();
    winrt::check_hresult(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
    winrt::com_ptr<::IInspectable> device;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(device))));
    return device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}
WinGCCaptureStrategy::WinGCCaptureStrategy() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // 1. 初始化 D3D11 设备
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, d3d11Device.GetAddressOf(), nullptr, d3d11Context.GetAddressOf()));
    device = CreateDirect3DDevice(d3d11Device.Get());
}
WinGCCaptureStrategy::~WinGCCaptureStrategy() {
    cleanup();
}
void WinGCCaptureStrategy::cleanup() {
    if (session) session.Close();
    if (framePool) framePool.Close();
    session = nullptr;
    framePool = nullptr;
    item = nullptr;
    stagingTexture = nullptr;
    currentHwnd = nullptr;
}
bool WinGCCaptureStrategy::initWinGC(HWND hwnd) {
    try {
        cleanup();
        currentHwnd = hwnd;
        item = CreateCaptureItemForWindow(hwnd);
        auto size = item.Size();
        width = size.Width;
        height = size.Height;
        // 创建帧池 (只保留 1 帧，保证实时性)
        framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);

        session = framePool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false); // 隐藏鼠标，建图不需要
        // --- 添加下面这一行来移除黄色边框 ---
        session.IsBorderRequired(false);
        session.StartCapture();
        return true;
    }
    catch (winrt::hresult_error const& ex) {
        // 只打印十六进制错误码，不打印 Message，避免宽字符转换导致的 strlen 崩溃
        unsigned int code = (unsigned int)ex.code().value;
        LOG_ERR("WinGC: Init Failed. HRESULT: 0x%08X", code);
        return false;
    }
    catch (...) {
        LOG_ERR("WinGC: 发生未知异常");
        return false;
    }
}
bool WinGCCaptureStrategy::capture(HWND hwnd, cv::Mat& result) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    // 如果窗口变了，重新初始化
    if (hwnd != currentHwnd || !session) {
        if (!initWinGC(hwnd)) return false;
    }
    try {
        // 尝试从池中拉取最新帧
        auto frame = framePool.TryGetNextFrame();
        if (!frame) return false;
        auto frameSurface = frame.Surface();
        auto surfaceInterface = frameSurface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(surfaceInterface->GetInterface(IID_PPV_ARGS(&texture)));
        copyTextureToMat(texture, result);
        return true;
    }
    catch (...) {
        return false;
    }
}
void WinGCCaptureStrategy::copyTextureToMat(const Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture, cv::Mat& mat) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    // 创建一个 staging 纹理用于将数据从显存拷到内存
    if (!stagingTexture || width != desc.Width || height != desc.Height) {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        winrt::check_hresult(d3d11Device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf()));
        width = desc.Width;
        height = desc.Height;
    }
    d3d11Context->CopyResource(stagingTexture.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(d3d11Context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        // WinGC 默认是 BGRA
        cv::Mat bgra(height, width, CV_8UC4, mapped.pData, mapped.RowPitch);
        cv::cvtColor(bgra, mat, cv::COLOR_BGRA2BGR); // 转换为 BGR
        d3d11Context->Unmap(stagingTexture.Get(), 0);
    }
}


// ==========================================
// ScreenGrabber 主类实现
// ==========================================

ScreenGrabber::ScreenGrabber() {
    // 构造时初始化一个默认的
    strategy = std::make_unique<GDICaptureStrategy>();
}

ScreenGrabber::~ScreenGrabber() {
    // unique_ptr 会自动释放 strategy
}

void ScreenGrabber::setConfig(const CaptureConfig& config)
{
    // 如果配置没变，直接返回
    if (this->activeConfig == config) return;
    // 配置变了，重置策略
    this->activeConfig = config;

    switch (config.Method) {
    case CaptureMethod::GDI:
        this->strategy = std::make_unique<GDICaptureStrategy>();
        break;
    case CaptureMethod::DirectX:
        // TODO: 实现 DX 截图策略
        // 暂时回退到 GDI
        this->strategy = std::make_unique<GDICaptureStrategy>();
        break;
    case CaptureMethod::WinGC:
        // TODO: 实现 WinGC 截图策略
        // 暂时回退到 GDI
        this->strategy = std::make_unique<WinGCCaptureStrategy>();
        break;
    }
}






std::shared_ptr<cv::Mat> ScreenGrabber::grab() {
    if (!strategy) return nullptr;

    auto& ctx = SharedContext::getInstance();

    //每次建图检查截图配置是否改变，原子变量几乎无性能开销
    if (ctx.getCaptureConfigVersion() != localConfigVersion) {
        CaptureConfig newConfig = ctx.getCurrentCaptureConfig(); // 只有版本号不同才加锁操作 读取配置
        setConfig(newConfig); // 更新 grabber 内部策略
        localConfigVersion = ctx.getCaptureConfigVersion(); // 更新本地版本
    }
    if (!activeConfig.targetHwnd) return nullptr;
    cv::Mat rawImg;
    if (strategy->capture(activeConfig.targetHwnd, rawImg)) {
        if (rawImg.empty()) return nullptr;
        // 成功，返回 shared_ptr
        // 使用 make_shared 创建，引用计数 +1
        return std::make_shared<cv::Mat>(rawImg);
    }

    // 截图失败（可能是窗口最小化了，或者被遮挡）//日志系统待接入
    return nullptr;
}
