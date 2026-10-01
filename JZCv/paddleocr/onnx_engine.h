// 极简 ONNX Runtime 推理封装（替换原来的 cv::dnn / JZModelEngineOnnx）
//
// ⚠ 这里**只用 ONNX Runtime 的 C API**(onnxruntime_c_api.h), 不用 C++ API(Ort::Session)。
//   原因: 本工程当前工具集是 VS2015(MSVC v140 / C++14), 而 ORT 1.30 的 C++ 头是 C++17:
//     * onnxruntime_cxx_api.h 无条件 #include <variant>, 且用了 `if constexpr`;
//     * onnxruntime_float16.h 也用了 `if constexpr`;
//   这些是语言级依赖, 用宏补不回来。C API 头则是一份纯 C 头, 唯一障碍是它把 NO_EXCEPTION
//   定义成了 `noexcept` 并用在函数指针 typedef 里(v140 报 C2279) —— 那处用宏能补,
//   见 onnx_engine.cpp 顶部。
//   将来工具集升到 VS2019+ 想换回 Ort::Session, 只需重写这两个文件, 本头接口不用动。
//
// ORT 类型(OrtEnv/OrtSession/OrtValue...)不外泄到公共头: 实现全部收在 .cpp 里(PIMPL)。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace PaddleOCR
{

    class OnnxEngine
    {
    public:
        OnnxEngine();
        ~OnnxEngine();
        OnnxEngine(const OnnxEngine &) = delete;
        OnnxEngine &operator=(const OnnxEngine &) = delete;

        // num_threads <= 0 表示交给 ORT 自己决定
        bool LoadModel(const std::string &model_path, int num_threads = 0);

        bool IsLoaded() const;

        const std::vector<std::string> &InputNames() const;
        const std::vector<std::string> &OutputNames() const;

        // blob 为连续内存的 float 数据，shape 为 NCHW（支持动态维度）。
        // 返回第 out_idx 个输出的全部数据，out_shape 回填其形状。
        std::vector<float> Run(const float *data, const std::vector<int64_t> &shape,
                               std::vector<int64_t> &out_shape, size_t out_idx = 0);

        // 便捷重载：直接用 cv::Mat(CV_32F, {N,C,H,W}) 作为输入
        std::vector<float> Run(const cv::Mat &blob, std::vector<int64_t> &out_shape,
                               size_t out_idx = 0);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace PaddleOCR
