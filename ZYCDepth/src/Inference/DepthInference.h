#pragma once
#include <opencv2/opencv.hpp>
#include <memory>
#include <onnxruntime_cxx_api.h>

struct DepthResult {
    cv::Mat depthMap;      // 原始深度数据 (CV_32F)
    cv::Mat visualDepth;   // 可视化深度图 (CV_8U)

    // V3 新增输出
    cv::Mat intrinsics;    // 3x3 相机内参 (fx, fy, cx, cy)
    cv::Mat extrinsics;    // 3x4 相机外参 (R|t)

    double inferTimeMs;    // 推理耗时
    bool isValid = false;
};

// 抽象接口 准备ONNX Runtime和TensorRT两种实现方法，TensorRT性能比ONNX高，但是配置麻烦，待议
class IDepthInference {
public:
    virtual ~IDepthInference() = default;
    virtual bool init(const std::string& modelPath) = 0;
    virtual DepthResult predict(const cv::Mat& input) = 0;
};

// ONNX Runtime 实现
class ONNXDepthInference : public IDepthInference {
public:
    ONNXDepthInference();
    bool init(const std::string& modelPath) override;
    DepthResult predict(const cv::Mat& input) override;
private:
    Ort::Env env;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    // V3 默认分辨率通常为 504 (根据你的导出脚本)
    int netWidth = 504;
    int netHeight = 504;
    // 修改输入输出节点名，对应onnx Python 导出脚本
    std::vector<const char*> inputNames = { "image" };
    // 顺序必须与导出时的 output_names 一致: ["depth", "intrinsics", "extrinsics"]
    std::vector<const char*> outputNames = { "depth", "intrinsics", "extrinsics" };
};