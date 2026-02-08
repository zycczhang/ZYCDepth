#include "DepthInference.h"
#include <chrono>
#include"Log/Logger.h"
ONNXDepthInference::ONNXDepthInference() : env(ORT_LOGGING_LEVEL_ERROR, "DepthAnythingV3") {}

bool ONNXDepthInference::init(const std::string& modelPath) {
    bool cudaEnabled = false; // 增加一个标志位
    try {
        // 1. 尝试配置 CUDA
        try {
            OrtCUDAProviderOptions cuda_options;
            // 设置一些常用的 CUDA 优化参数（可选）
            cuda_options.device_id = 0;
            cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;

            sessionOptions.AppendExecutionProvider_CUDA(cuda_options);
            cudaEnabled = true; // 如果运行到这里没报错，说明 Provider 加载成功
        }
        catch (...) {
            LOG_WARN("CUDA 硬件环境检查失败，系统将自动回退到 CPU 模式", true);
            cudaEnabled = false;
        }

        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // 2. 加载模型
#ifdef _WIN32
        std::wstring wModelPath = std::wstring(modelPath.begin(), modelPath.end());
        session = std::make_unique<Ort::Session>(env, wModelPath.c_str(), sessionOptions);
#else
        session = std::make_unique<Ort::Session>(env, modelPath.c_str(), sessionOptions);
#endif

        // 3. 根据标志位输出成功日志
        if (cudaEnabled) {
            LOG_INFO("深度估计模型已成功加载 [推理引擎: CUDA/GPU]", true);
        }
        else {
            LOG_INFO("深度估计模型已成功加载 [推理引擎: CPU]", true);
        }

        return true;
    }
    catch (const Ort::Exception& e) {
        LOG_ERR("模型初始化失败: " + std::string(e.what()), true);
        return false;
    }
}

DepthResult ONNXDepthInference::predict(const cv::Mat& input) {
    if (input.empty()) return DepthResult();

    auto start = std::chrono::high_resolution_clock::now();
    DepthResult result;
    int origW = input.cols;
    int origH = input.rows;
    // --- 1. 预处理 ---
    cv::Mat resized, floatImg;
    cv::resize(input, resized, cv::Size(netWidth, netHeight));
    cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB); // 必须转 RGB
    resized.convertTo(floatImg, CV_32FC3, 1.0 / 255.0);
    // 标准化
    cv::Scalar mean(0.485, 0.456, 0.406);
    cv::Scalar std(0.229, 0.224, 0.225);
    // 注意：这里由于是矩阵运算，可以直接 subtract/divide
    cv::subtract(floatImg, mean, floatImg);
    cv::divide(floatImg, std, floatImg);
    // HWC -> CHW
    std::vector<float> inputTensorValues(1 * 3 * netHeight * netWidth);
    std::vector<cv::Mat> channels(3);
    for (int i = 0; i < 3; ++i) {
        channels[i] = cv::Mat(netHeight, netWidth, CV_32FC1, &inputTensorValues[i * netHeight * netWidth]);
    }
    cv::split(floatImg, channels);
    // --- 2. 运行推理 ---
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> inputShape = { 1, 3, netHeight, netWidth };
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memory_info, inputTensorValues.data(), inputTensorValues.size(), inputShape.data(), inputShape.size());
    // 运行模型，获取 3 个输出 Tensor
    auto outputTensors = session->Run(
        Ort::RunOptions{ nullptr },
        inputNames.data(), &inputTensor, 1,
        outputNames.data(), outputNames.size()
    );
    // --- 3. 后处理 ---

    // A. 提取深度图 (Output 0: [1, 504, 504])
    float* depthData = outputTensors[0].GetTensorMutableData<float>();
    cv::Mat rawDepth(netHeight, netWidth, CV_32FC1, depthData);
    result.depthMap = rawDepth.clone(); // 建议 resize 回原图大小或保持 504
    // B. 提取内参并还原缩放 (Output 1: [3, 3])
    float* kData = outputTensors[1].GetTensorMutableData<float>();
    cv::Mat K = cv::Mat(3, 3, CV_32FC1, kData).clone();

    // 关键：将 504 空间的内参映射回原图尺寸
    float scaleX = (float)origW / netWidth;
    float scaleY = (float)origH / netHeight;
    K.at<float>(0, 0) *= scaleX; // fx
    K.at<float>(0, 2) *= scaleX; // cx
    K.at<float>(1, 1) *= scaleY; // fy
    K.at<float>(1, 2) *= scaleY; // cy
    result.intrinsics = K;
    // C. 提取外参 (Output 2: [3, 4])
    float* rtData = outputTensors[2].GetTensorMutableData<float>();
    result.extrinsics = cv::Mat(3, 4, CV_32FC1, rtData).clone();
    // D. 生成可视化图
    double minV, maxV;
    cv::minMaxLoc(result.depthMap, &minV, &maxV);
    result.depthMap.convertTo(result.visualDepth, CV_8UC1, 255.0 / (maxV - minV), -minV * 255.0 / (maxV - minV));
    cv::applyColorMap(result.visualDepth, result.visualDepth, cv::COLORMAP_INFERNO);
    auto end = std::chrono::high_resolution_clock::now();
    result.inferTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    result.isValid = true;
    return result;
}