#include "CommonTypes.h"
#include "Log/Logger.h"
#include"WebSocket/WebSocketServer.h"
/**
 * @brief 获取SharedContext单例实例
 * @details C++11及以上保证局部静态变量初始化线程安全，实现饿汉式单例
 * @return SharedContext& 全局唯一的共享上下文实例
 */
SharedContext& SharedContext::getInstance()
{
    static SharedContext instance;
    return instance;
}

CaptureConfig SharedContext::getCurrentCaptureConfig() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return currentCaptureConfig;
}

void SharedContext::setCurrentCaptureConfig(CaptureConfig config)
{
    std::lock_guard<std::mutex> lock(mtx);

    // 1. 将网页传来的 UTF-8 窗口名转回 ANSI (GBK)，否则 FindWindowA 找不到中文标题
    std::string ansiWindowName = Utf8ToGbk(config.targetWindowName);

    HWND foundHwnd = FindWindowA(NULL, ansiWindowName.c_str());
    config.targetHwnd = foundHwnd;

    // 2. 只有当整个配置（包括 Method, Name, Hwnd）发生变化时才更新版本号
    if (this->currentCaptureConfig != config) {
        this->currentCaptureConfig = config;
        this->configVersion++; // 触发版本号增加，通知截图线程更新策略

        if (foundHwnd) {
            LOG_INFO("配置已更新: 窗口[" + config.targetWindowName + "] 匹配成功，句柄: " + std::to_string((long long)foundHwnd), true);
        }
        else {
            LOG_WARN("配置已更新: 窗口[" + config.targetWindowName + "] 未找到", true);
        }
    }
}

uint64_t SharedContext::getCaptureConfigVersion() const {
    return configVersion.load(std::memory_order_acquire);
}



// ========== 建图状态 访问接口 ==========
/**
 * @brief 获取建图状态
 * @return bool true-建图中，false-停止建图
 */
bool SharedContext::getIsMapping() const
{
    return isMapping;
}

/**
 * @brief 设置建图状态
 * @param state 目标建图状态（true-开始建图，false-停止建图）
 * @details 状态变更时唤醒所有阻塞在cv_new_frame的线程，避免线程永久阻塞
 */
void SharedContext::setIsMapping(bool state)
{
    isMapping = state;
    cv_new_frame.notify_all(); // 状态改变时唤醒所有卡住的线程（关键）
}

// ========== 帧数据 写入接口（截图线程专用） ==========
/**
 * @brief 写入当前帧数据（截图线程专用）
 * @param frame 待写入的帧数据（右值引用）
 * @details 1. 移动语义写入，无图像拷贝开销
 *          2. 序列号防回退检查：仅当新帧序列号>当前帧时才更新
 *             （避免截图线程异常导致序列号回退，引发建图线程永久阻塞）
 *          3. 锁外通知等待线程：避免等待线程唤醒后竞争锁，提升并发效率
 *          4. 即使新帧被丢弃，少量无效通知无影响（等待条件会再次校验）
 */
void SharedContext::setCurrentFrame(FrameData&& frame)
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        // 序列号防回退检查：仅更新更大序列号的帧
        if (frame.sequenceID > currentFrame.sequenceID)
        {
            currentFrame = std::move(frame);
        }
        // 若新帧序列号<=当前帧，直接丢弃，不更新、不通知
    }
    // 锁外通知：提升并发效率
    cv_new_frame.notify_all();
}

// ========== 帧数据 读取接口（非阻塞，Web GUI/显示模块专用） ==========
/**
 * @brief 非阻塞读取当前帧数据
 * @return FrameData 当前帧数据拷贝
 * @details 拷贝仅包含智能指针+double+long long，性能开销可忽略
 *          适用于Web GUI、显示模块等非实时性读取场景
 */
FrameData SharedContext::getCurrentFrame() const
{
    std::lock_guard<std::mutex> lock(mtx);
    return currentFrame;
}

// ========== 帧数据 阻塞读取接口（建图算法专用，核心接口） ==========
/**
 * @brief 阻塞等待新帧数据（建图算法专用）
 * @param lastID 上一次处理的帧序列号
 * @return FrameData 新帧数据（停止建图返回空帧）
 * @details 1. 阻塞条件：有新帧（sequenceID > lastID） 或 停止建图（isMapping=false）
 *          2. 停止建图时返回空帧（image=null，sequenceID=-1）
 *          3. 保证建图算法只处理递增的有效帧
 */
FrameData SharedContext::waitForNewFrame(long long lastID)
{
    std::unique_lock<std::mutex> lock(mtx);
    cv_new_frame.wait(lock, [this, lastID]
        {
            return (!currentFrame.empty() && currentFrame.sequenceID > lastID) || !isMapping;
        });

    // 停止推理返回空帧
    if (!isInferencing)
    {
        return FrameData(); // 空帧：image=null，sequenceID=-1
    }

    // 有新帧时返回当前帧
    return currentFrame;
}

void SharedContext::setCurrentDepthFrame(FrameData&& frame) {
    std::lock_guard<std::mutex> lock(mtx);
    currentDepthFrame = std::move(frame);
}

FrameData SharedContext::getCurrentDepthFrame() const {
    std::lock_guard<std::mutex> lock(mtx);
    return currentDepthFrame;
}

bool SharedContext::getIsInferencing() const
{
    return isInferencing;
}

void SharedContext::setIsInferencing(bool state)
{
    cv_new_frame.notify_all(); // 状态改变时唤醒所有卡住的线程（关键）
    isInferencing = state;
}



std::string SharedContext::Utf8ToGbk(const std::string& strUtf8) {
    if (strUtf8.empty()) return "";

    // 1. UTF-8 -> WideChar (Unicode)
    // 获取转换所需的缓冲区长度
    int len = MultiByteToWideChar(CP_UTF8, 0, strUtf8.c_str(), -1, NULL, 0);
    if (len <= 0) return "";

    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, strUtf8.c_str(), -1, &wstr[0], len);

    // 2. WideChar -> ANSI (在中文系统下即为 GBK)
    // 获取转换所需的缓冲区长度
    int gbkLen = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (gbkLen <= 0) return "";

    std::string strGbk(gbkLen, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &strGbk[0], gbkLen, NULL, NULL);

    // 移除 WideCharToMultiByte 可能自动添加的末尾空字符 \0
    if (!strGbk.empty() && strGbk.back() == '\0') {
        strGbk.pop_back();
    }

    return strGbk;
}

// 辅助函数：将 GBK (ANSI) 转换为 UTF-8
std::string SharedContext::GbkToUtf8(const std::string& strGbk) {
    if (strGbk.empty()) return "";

    // 1. GBK -> WideChar (Unicode)
    int len = MultiByteToWideChar(CP_ACP, 0, strGbk.c_str(), -1, NULL, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_ACP, 0, strGbk.c_str(), -1, &wstr[0], len);

    // 2. WideChar -> UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string strUtf8(utf8Len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &strUtf8[0], utf8Len, NULL, NULL);

    // 移除 WideCharToMultiByte 自动添加的末尾 \0
    if (!strUtf8.empty() && strUtf8.back() == '\0') strUtf8.pop_back();

    return strUtf8;
}
