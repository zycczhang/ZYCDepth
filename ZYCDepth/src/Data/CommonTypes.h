#pragma once
#include <mutex>
#include <string>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <opencv2/opencv.hpp>
#include <windows.h>
/**
 * @brief 截图方法枚举类型
 * @details 支持三种主流Windows窗口截图方式，适配不同场景的性能/兼容性需求
 */
enum class CaptureMethod
{
    GDI,        ///< GDI截图：兼容性最好，性能中等
    DirectX,    ///< DirectX截图：性能最优，适配DirectX渲染窗口
    WinGC       ///< WinGC截图：适配Windows Graphics Capture接口（Win10+）
};

/**
 * @brief 截图配置
 */
struct CaptureConfig
{
    CaptureMethod Method = CaptureMethod::WinGC;  ///< 当前截图方法，默认GDI（兼容性优先）
    std::string targetWindowName = "GameProcess";      ///< 目标截图窗口名称，默认"GameProcess"
    HWND targetHwnd = nullptr;      ///< 目标窗口句柄
    int captureFps = 30;//截图频率

    // 重载!= 运算符
    bool operator!=(const CaptureConfig& other) const {
        return (this->Method != other.Method) ||
            (this->targetWindowName != other.targetWindowName) ||
            (this->targetHwnd != other.targetHwnd) || 
            (this->captureFps != other.captureFps);
    }

    // 重载== 运算符（!=的反向逻辑，保证运算符完整性）
    bool operator==(const CaptureConfig& other) const {
        return !(*this != other); // 复用!=逻辑，避免重复代码，减少维护成本
    }

};

/**
 * @brief 帧数据结构
 * @details 封装图像数据、时间戳、序列号，用于多模块跨线程共享帧数据
 *          采用智能指针避免图像拷贝，序列号保证帧的递增唯一性
 */
struct FrameData {
    std::shared_ptr<cv::Mat> image;      // 对于原图是 BGR，对于深度图是可视化图
    std::shared_ptr<cv::Mat> rawDepth;   // [新增] 原始 float32 深度数据
    cv::Mat intrinsics;                  // [新增] 3x3 内参
    cv::Mat extrinsics;                  // [新增] 3x4 外参

    long long sequenceID = -1;
    double timestamp = 0.0;
    double captureDurationMs = 0.0;
    inline bool empty() const { return !image || image->empty(); }
};

/**
 * @brief 多模块共享上下文类
 * @details 单例模式，线程安全，封装所有全局共享状态
 *          核心业务流程：截图线程生产帧 → 建图线程阻塞消费新帧 → Web GUI/显示模块非阻塞读取帧
 */
class SharedContext
{
private:
    /**
     * @brief 私有构造函数
     * @details 禁止外部实例化，保证单例模式的唯一性
     */
    SharedContext() = default;

    /**
     * @brief 禁用拷贝/赋值/移动语义
     * @details 避免浅拷贝导致的多实例问题，保证全局唯一实例
     */
    SharedContext(const SharedContext&) = delete;
    SharedContext& operator=(const SharedContext&) = delete;
    SharedContext(SharedContext&&) = delete;
    SharedContext& operator=(SharedContext&&) = delete;

    // 共享状态成员（私有，仅通过加锁接口访问）
    mutable std::mutex mtx;                  ///< 可变互斥锁：支持const成员函数中加锁操作
    std::condition_variable cv_new_frame;    ///< 条件变量：截图线程通知新帧，解决建图线程忙等待问题
    std::atomic<uint64_t> configVersion{ 0 }; // <--- 截图配置版本号（原子变量）用来判断配置是否更改过
    CaptureConfig currentCaptureConfig;     ///< 截图配置
    std::atomic<bool> isMapping = false;               ///< 建图状态（原子变量）：true-建图中，false-停止建图
    std::atomic<bool> isInferencing = false;               ///< 建图状态（原子变量）：true-建图中，false-停止建图
    FrameData currentFrame;                             ///< 核心共享数据：当前帧（截图线程写入，多模块读取）
    FrameData currentDepthFrame; // 存储最新的深度估计结果
    std::atomic<double> lastCaptureTimeMs{ 0.0 };
    std::atomic<double> lastInferenceTimeMs{ 0.0 };

public:
    /**
     * @brief 线程安全的单例获取接口（饿汉式）
     * @details C++11及以上标准保证：局部静态变量初始化线程安全，仅初始化一次，全局唯一
     * @return SharedContext& 全局唯一的共享上下文实例
     */
    static SharedContext& getInstance();

    /**
     * @brief 获取截图配置
     */
    CaptureConfig getCurrentCaptureConfig() const;

    /**
     * @brief 设置截图配置
     */
    void setCurrentCaptureConfig(CaptureConfig config);

    uint64_t getCaptureConfigVersion() const;

    // ========== 建图状态 访问接口 ==========
    /**
     * @brief 获取建图状态
     * @return bool true-建图中，false-停止建图
     */
    bool getIsMapping() const;

    /**
     * @brief 设置建图状态
     * @param state 目标建图状态（true-开始建图，false-停止建图）
     * @details 状态变更时会唤醒所有阻塞在cv_new_frame的线程，避免线程永久阻塞
     */
    void setIsMapping(bool state);

    // ========== 帧数据 写入接口（截图线程专用） ==========
    /**
     * @brief 写入当前帧数据（截图线程专用）
     * @param frame 待写入的帧数据（右值引用，移动语义）
     * @details 1. 移动语义写入，无图像拷贝开销
     *          2. 序列号防回退检查：仅当新帧序列号>当前帧时才更新
     *          3. 锁外通知等待线程，提升并发效率
     */
    void setCurrentFrame(FrameData&& frame);

    // ========== 帧数据 读取接口（非阻塞，Web GUI/显示模块专用） ==========
    /**
     * @brief 非阻塞读取当前帧数据
     * @return FrameData 当前帧数据拷贝（仅包含智能指针+基础类型，开销可忽略）
     * @details 适用于Web GUI、显示模块等非实时性读取场景，不阻塞调用线程
     */
    FrameData getCurrentFrame() const;

    // ========== 帧数据 阻塞读取接口（算法专用，核心接口） ==========
    /**
     * @brief 阻塞等待新帧数据（算法专用）
     * @param lastID 上一次处理的帧序列号
     * @return FrameData 新帧数据（若停止建图则返回空帧）
     * @details 1. 阻塞直到有新帧（sequenceID > lastID）或停止建图
     *          2. 停止建图时返回空帧（image=null，sequenceID=-1）
     *          3. 是算法消费新帧的核心接口，保证帧的时序性
     */
    FrameData waitForNewFrame(long long lastID);

    void setCurrentDepthFrame(FrameData&& frame);

    FrameData getCurrentDepthFrame() const;

    bool getIsInferencing() const;

    void setIsInferencing(bool state);

    static std::string Utf8ToGbk(const std::string& strUtf8);

    static std::string GbkToUtf8(const std::string& strGbk);

    void setCaptureTime(double ms) { lastCaptureTimeMs = ms; }
    double getCaptureTime() const { return lastCaptureTimeMs.load(); }
    void setInferenceTime(double ms) { lastInferenceTimeMs = ms; }
    double getInferenceTime() const { return lastInferenceTimeMs.load(); }
};